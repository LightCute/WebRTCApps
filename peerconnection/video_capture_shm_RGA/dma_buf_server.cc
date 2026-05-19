// dma_buf_server.cc — Unix socket server to pass dma-buf fds
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/video_capture_shm_RGA/dma_buf_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int SendFds(int client_fd, const int* fds, int num_fds) {
  // SCM_RIGHTS: send file descriptors over Unix socket
  // Piggyback on a dummy 1-byte message
  char dummy = 'F';
  struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

  // cmsg buffer: one fd per slot
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
  // Copy args for thread
  std::string path_copy = socket_path;
  std::vector<int> fd_copy(fds, fds + num_fds);

  std::thread([path_copy, fd_copy]() {
    // Remove stale socket file
    unlink(path_copy.c_str());

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
      perror("DmaBufServer: socket");
      return;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path_copy.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      perror("DmaBufServer: bind");
      close(server_fd);
      return;
    }

    if (listen(server_fd, 1) < 0) {
      perror("DmaBufServer: listen");
      close(server_fd);
      unlink(path_copy.c_str());
      return;
    }

    fprintf(stderr, "DmaBufServer: waiting for consumer on %s...\n",
            path_copy.c_str());

    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
      perror("DmaBufServer: accept");
      close(server_fd);
      unlink(path_copy.c_str());
      return;
    }

    if (SendFds(client_fd, fd_copy.data(), (int)fd_copy.size()) < 0) {
      fprintf(stderr, "DmaBufServer: sendmsg failed: %s\n", strerror(errno));
    } else {
      fprintf(stderr, "DmaBufServer: sent %d fds to consumer\n",
              (int)fd_copy.size());
    }

    close(client_fd);
    close(server_fd);
    unlink(path_copy.c_str());
  }).detach();

  return 0;
}

void DmaBufServer::Stop() {
  // Thread detaches itself; socket cleanup happens after consumer connects
}
