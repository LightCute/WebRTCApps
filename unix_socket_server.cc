// unix_socket_server.cc
#include "unix_socket_server.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "apps/peerconnection/client/webrtc_engine.h"
#include "json/reader.h"
#include "json/value.h"
#include "json/writer.h"
#include "rtc_base/logging.h"

UnixSocketServer::UnixSocketServer(const std::string& socket_path,
                                   WebRTCEngine* engine)
    : socket_path_(socket_path), engine_(engine) {}

UnixSocketServer::~UnixSocketServer() { Stop(); }

void UnixSocketServer::Start() {
  running_ = true;
  io_thread_ = std::thread(&UnixSocketServer::IoLoop, this);
}

void UnixSocketServer::Stop() {
  running_ = false;
  shutdown_cv_.notify_all();
  if (client_fd_ >= 0) {
    close(client_fd_);
    client_fd_ = -1;
  }
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    unlink(socket_path_.c_str());
  }
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
}

void UnixSocketServer::Wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  shutdown_cv_.wait(lock, [this] { return !running_; });
}

void UnixSocketServer::SendToClient(const std::string& json) {
  if (client_fd_ < 0) return;
  std::string line = json + "\n";
  write(client_fd_, line.c_str(), line.size());
}

void UnixSocketServer::SendJson(int fd, const std::string& json) {
  std::string line = json + "\n";
  write(fd, line.c_str(), line.size());
}

void UnixSocketServer::SendResponse(int id, bool ok, const std::string& error) {
  Json::Value resp;
  resp["id"] = id;
  resp["ok"] = ok;
  if (!error.empty()) resp["error"] = error;
  Json::StreamWriterBuilder factory;
  factory["indentation"] = "";
  SendJson(client_fd_, Json::writeString(factory, resp));
}

void UnixSocketServer::HandleCommand(const std::string& raw_json, int client_fd) {
  Json::Value root;
  Json::CharReaderBuilder factory;
  std::string errors;
  std::istringstream stream(raw_json);
  if (!Json::parseFromStream(factory, stream, &root, &errors)) {
    RTC_LOG(LS_WARNING) << "Failed to parse JSON command: " << errors;
    return;
  }

  std::string cmd = root.get("cmd", "").asString();
  int id = root.get("id", -1).asInt();

  if (cmd == "connect") {
    std::string server = root["params"].get("server", "").asString();
    int port = root["params"].get("port", 0).asInt();
    if (server.empty() || port == 0) {
      SendResponse(id, false, "Missing server or port");
      return;
    }
    engine_->signaling_thread()->PostTask([this, id, server, port] {
          engine_->ConnectToServer(server, port);
          SendResponse(id, true);
      });
  } else if (cmd == "disconnect") {
    engine_->signaling_thread()->PostTask([this, id] {
        engine_->DisconnectFromServer();
        SendResponse(id, true);
      });
  } else if (cmd == "call") {
    int peer_id = root["params"].get("peer_id", -1).asInt();
    if (peer_id < 0) {
      SendResponse(id, false, "Invalid peer_id");
      return;
    }
    engine_->signaling_thread()->PostTask([this, id, peer_id] {
        engine_->ConnectToPeer(peer_id);
        SendResponse(id, true);
      });
  } else if (cmd == "hangup") {
    engine_->signaling_thread()->PostTask([this, id] {
        engine_->HangUp();
        SendResponse(id, true);
      });
  } else if (cmd == "shutdown") {
    SendResponse(id, true);
    running_ = false;
    shutdown_cv_.notify_all();
  } else if (cmd == "set_mute") {
    bool audio_mute = root["params"].get("audio", false).asBool();
    bool video_mute = root["params"].get("video", false).asBool();
    engine_->signaling_thread()->PostTask([this, audio_mute, video_mute] {
        if (audio_mute) engine_->SetAudioMuted(true);
        if (video_mute) engine_->SetVideoPaused(true);
      });
    SendResponse(id, true);
  } else if (cmd == "send_data") {
    std::string text = root["params"].get("text", "").asString();
    engine_->signaling_thread()->PostTask([this, id, text] {
        engine_->SendData(text);
        SendResponse(id, true);
      });
  } else {
    SendResponse(id, false, "Unknown command: " + cmd);
  }
}

void UnixSocketServer::IoLoop() {
  unlink(socket_path_.c_str());

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    RTC_LOG(LS_ERROR) << "Failed to create Unix socket";
    return;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    RTC_LOG(LS_ERROR) << "Failed to bind Unix socket: " << strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  if (listen(listen_fd_, 1) < 0) {
    RTC_LOG(LS_ERROR) << "Failed to listen on Unix socket";
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    RTC_LOG(LS_ERROR) << "Failed to create epoll";
    return;
  }

  struct epoll_event ev = {};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd_;
  epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd_, &ev);

  char buf[4096];
  std::string line_buf;

  while (running_) {
    struct epoll_event events[2];
    int nfds = epoll_wait(epfd, events, 2, 100);

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;

      if (fd == listen_fd_) {
        client_fd_ = accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ >= 0) {
          struct epoll_event cev = {};
          cev.events = EPOLLIN | EPOLLRDHUP;
          cev.data.fd = client_fd_;
          epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd_, &cev);
          RTC_LOG(LS_INFO) << "Unix socket client connected";
        }
      } else {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
          close(fd);
          client_fd_ = -1;
          RTC_LOG(LS_INFO) << "Unix socket client disconnected";
          continue;
        }
        buf[n] = '\0';
        line_buf.append(buf, n);

        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
          std::string line = line_buf.substr(0, pos);
          line_buf.erase(0, pos + 1);
          if (!line.empty()) {
            HandleCommand(line, fd);
          }
        }
      }
    }
  }

  if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
  close(epfd);
  close(listen_fd_);
  listen_fd_ = -1;
  unlink(socket_path_.c_str());
}

#pragma GCC diagnostic pop
