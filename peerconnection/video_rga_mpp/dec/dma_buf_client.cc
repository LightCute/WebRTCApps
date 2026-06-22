#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/video_rga_mpp/dec/dma_buf_client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

DmaBufClient::~DmaBufClient() {
  if (sock_fd_ >= 0) close(sock_fd_);
}

std::vector<int> DmaBufClient::ReceiveFds(const std::string& socket_path,
                                          int expected_count) {
  std::vector<int> result;
  sock_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd_ < 0) { perror("DmaBufClient: socket"); return result; }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  for (int retry = 0; retry < 30; ++retry) {
    if (connect(sock_fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0) break;
    usleep(100000);
  }

  char dummy;
  struct iovec iov = { &dummy, 1 };
  size_t cmsg_size = CMSG_SPACE(expected_count * sizeof(int));
  char* cmsg_buf = new char[cmsg_size]();
  struct msghdr msg = {};
  msg.msg_iov = &iov; msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf; msg.msg_controllen = cmsg_size;

  if (recvmsg(sock_fd_, &msg, 0) < 0) {
    perror("DmaBufClient: recvmsg");
    delete[] cmsg_buf;
    return result;
  }

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
    int* fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
    int num_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (int i = 0; i < num_fds; ++i) result.push_back(fds[i]);
  }
  delete[] cmsg_buf;
  return result;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
