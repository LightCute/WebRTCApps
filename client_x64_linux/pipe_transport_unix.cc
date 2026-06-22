// pipe_transport_unix.cc
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/client_x64_linux/pipe_transport_unix.h"

#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "rtc_base/logging.h"

UnixSocketTransport::UnixSocketTransport(const std::string& socket_path)
    : socket_path_(socket_path) {}

UnixSocketTransport::~UnixSocketTransport() { Stop(); }

bool UnixSocketTransport::Start(const std::string& /*endpoint*/) {
  running_ = true;
  io_thread_ = std::thread(&UnixSocketTransport::IoLoop, this);
  return true;
}

void UnixSocketTransport::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  shutdown_cv_.wait(lock, [this] { return !running_; });
}

void UnixSocketTransport::Send(const std::string& data) {
  if (client_fd_ < 0) return;
  std::string line = data + "\n";
  (void)write(client_fd_, line.c_str(), line.size());
}

void UnixSocketTransport::Stop() {
  running_ = false;
  shutdown_cv_.notify_all();
  if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
  if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
  unlink(socket_path_.c_str());
  if (io_thread_.joinable()) io_thread_.join();
}

void UnixSocketTransport::IoLoop() {
  unlink(socket_path_.c_str());
  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return;

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
      listen(listen_fd_, 1) < 0) {
    close(listen_fd_); listen_fd_ = -1; return;
  }

  int epfd = epoll_create1(0);
  struct epoll_event ev = {};
  ev.events = EPOLLIN; ev.data.fd = listen_fd_;
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
          if (on_connected) on_connected();
        }
      } else {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
          close(fd); client_fd_ = -1;
          if (on_disconnected) on_disconnected();
          continue;
        }
        buf[n] = '\0';
        line_buf.append(buf, n);
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
          std::string line = line_buf.substr(0, pos);
          line_buf.erase(0, pos + 1);
          if (!line.empty() && on_message)
            on_message(line);
        }
      }
    }
  }
  if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
  close(epfd); close(listen_fd_); listen_fd_ = -1;
  unlink(socket_path_.c_str());
}

#pragma GCC diagnostic pop
