#include "apps/peerconnection/dc_call/unix_socket_server.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "rtc_base/logging.h"

UnixSocketServer::UnixSocketServer(const std::string& socket_path)
    : socket_path_(socket_path) {}

UnixSocketServer::~UnixSocketServer() {
  Disconnect();
}

bool UnixSocketServer::WaitForClient() {
  unlink(socket_path_.c_str());

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    RTC_LOG(LS_ERROR) << "Unix socket: failed to create: " << strerror(errno);
    return false;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    RTC_LOG(LS_ERROR) << "Unix socket: bind failed: " << strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (listen(listen_fd_, 1) < 0) {
    RTC_LOG(LS_ERROR) << "Unix socket: listen failed: " << strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  RTC_LOG(LS_INFO) << "Unix socket listening on " << socket_path_
                   << ", waiting for client...";
  client_fd_ = accept(listen_fd_, nullptr, nullptr);
  if (client_fd_ < 0) {
    RTC_LOG(LS_ERROR) << "Unix socket: accept failed: " << strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  RTC_LOG(LS_INFO) << "Unix socket client connected on fd=" << client_fd_;
  return true;
}

std::string UnixSocketServer::ReadLine() {
  if (client_fd_ < 0) return {};

  // Check if we already have a complete line in the buffer
  size_t pos = recv_buf_.find('\n');

  // Read in bulk until we get a complete line
  while (pos == std::string::npos) {
    char tmp[4096];
    ssize_t n = read(client_fd_, tmp, sizeof(tmp));
    if (n <= 0) {
      RTC_LOG(LS_INFO) << "Unix socket: client disconnected during read";
      Disconnect();
      return {};
    }
    recv_buf_.append(tmp, n);
    pos = recv_buf_.find('\n');
  }

  // Extract line up to (but not including) newline
  std::string line = recv_buf_.substr(0, pos);
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  recv_buf_.erase(0, pos + 1);
  return line;
}

void UnixSocketServer::WriteLine(const std::string& line) {
  if (client_fd_ < 0) return;
  std::string msg = line + "\n";
  Write(msg);
}

void UnixSocketServer::Write(const std::string& data) {
  if (client_fd_ < 0) return;
  ssize_t written = write(client_fd_, data.c_str(), data.size());
  if (written < 0) {
    RTC_LOG(LS_WARNING) << "Unix socket: write failed: " << strerror(errno);
  }
}

void UnixSocketServer::Disconnect() {
  if (client_fd_ >= 0) {
    close(client_fd_);
    client_fd_ = -1;
    RTC_LOG(LS_INFO) << "Unix socket client disconnected";
  }
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    unlink(socket_path_.c_str());
  }
}
