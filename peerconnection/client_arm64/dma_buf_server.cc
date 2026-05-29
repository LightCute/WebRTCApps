// dma_buf_server.cc — Unix socket server to pass dma-buf fds to consumers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/client_arm64/dma_buf_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

static int SendFds(int client_fd, const int* fds, int num_fds) {
  char dummy = 'F';
  struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

  size_t cmsg_size = CMSG_SPACE(num_fds * sizeof(int));
  char* cmsg_buf = new char[cmsg_size];
  memset(cmsg_buf, 0, cmsg_size);

  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = cmsg_size;

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(num_fds * sizeof(int));
  memcpy(CMSG_DATA(cmsg), fds, num_fds * sizeof(int));

  ssize_t sent = sendmsg(client_fd, &msg, 0);
  delete[] cmsg_buf;
  return (sent >= 0) ? 0 : -errno;
}

DmaBufServer::~DmaBufServer() {
  Stop();
}

int DmaBufServer::Start(const std::string& socket_path, const int* fds,
                        int num_fds) {
  if (thread_.joinable()) return -1;  // already started

  unlink(socket_path.c_str());

  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    perror("DmaBufServer: socket");
    return -1;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("DmaBufServer: bind");
    close(server_fd_); server_fd_ = -1;
    return -1;
  }

  // Backlog 5: renderer + AI + extras. accept() blocks with zero CPU.
  if (listen(server_fd_, 5) < 0) {
    perror("DmaBufServer: listen");
    close(server_fd_); server_fd_ = -1;
    unlink(socket_path.c_str());
    return -1;
  }

  std::string path_copy = socket_path;
  std::vector<int> fd_copy(fds, fds + num_fds);
  int fd = server_fd_;  // capture for lambda

  thread_ = std::thread([path_copy, fd_copy, fd]() {
    fprintf(stderr, "DmaBufServer: listening on %s (%d fds)\n",
            path_copy.c_str(), (int)fd_copy.size());

    while (true) {
      int client_fd = accept(fd, nullptr, nullptr);
      if (client_fd < 0) {
        if (errno == EBADF || errno == EINVAL) break;  // server shut down
        perror("DmaBufServer: accept");
        continue;
      }

      fprintf(stderr, "DmaBufServer: client connected, sending %d fds\n",
              (int)fd_copy.size());

      if (SendFds(client_fd, fd_copy.data(), (int)fd_copy.size()) < 0) {
        fprintf(stderr, "DmaBufServer: sendmsg failed: %s\n", strerror(errno));
      }

      close(client_fd);
      // Client disconnects after receiving fds — the fds remain valid
      // in the client process for the lifetime of the DMA-BUF pool.
    }
  });

  return 0;
}

void DmaBufServer::Stop() {
  if (server_fd_ >= 0) {
    shutdown(server_fd_, SHUT_RDWR);
    close(server_fd_);
    server_fd_ = -1;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
