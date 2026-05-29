// dma_buf_server.cc — multi-client poll loop for dma-buf fd handoff
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/video_capture_shm_RGA/dma_buf_server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

static int SendFdsWithMsg(int client_fd, const int* fds, int num_fds,
                          const HandshakeMsg& msg) {
  struct iovec iov = { .iov_base = const_cast<HandshakeMsg*>(&msg),
                        .iov_len = sizeof(msg) };
  size_t cmsg_size = CMSG_SPACE(num_fds * sizeof(int));
  char* cmsg_buf = new char[cmsg_size];
  memset(cmsg_buf, 0, cmsg_size);

  struct msghdr mhdr = {};
  mhdr.msg_iov = &iov;
  mhdr.msg_iovlen = 1;
  mhdr.msg_control = cmsg_buf;
  mhdr.msg_controllen = cmsg_size;

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&mhdr);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(num_fds * sizeof(int));
  memcpy(CMSG_DATA(cmsg), fds, num_fds * sizeof(int));

  ssize_t sent = sendmsg(client_fd, &mhdr, 0);
  delete[] cmsg_buf;
  if (sent != static_cast<ssize_t>(sizeof(msg))) {
    return (sent < 0) ? -errno : -EMSGSIZE;
  }
  return 0;
}

DmaBufServer::DmaBufServer() {
  for (int i = 0; i < MAX_CONSUMERS; ++i) client_fds_[i] = -1;
}

DmaBufServer::~DmaBufServer() { Stop(); }

int DmaBufServer::Start(const std::string& socket_path, const int* fds,
                        int num_fds, size_t frame_size, ShmMultiCtrlBlock* ctrl) {
  if (thread_.joinable()) return -1;

  socket_path_ = socket_path;
  dma_buf_fds_.assign(fds, fds + num_fds);
  frame_size_ = frame_size;
  ctrl_ = ctrl;

  unlink(socket_path_.c_str());
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) { perror("DmaBufServer: socket"); return -1; }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("DmaBufServer: bind"); close(server_fd_); server_fd_ = -1; return -1;
  }
  if (listen(server_fd_, 5) < 0) {
    perror("DmaBufServer: listen"); close(server_fd_); server_fd_ = -1;
    unlink(socket_path_.c_str()); return -1;
  }

  running_ = true;
  thread_ = std::thread(&DmaBufServer::RunLoop, this);
  fprintf(stderr, "DmaBufServer: listening on %s (multi-client)\n",
          socket_path_.c_str());
  return 0;
}

void DmaBufServer::Stop() {
  running_ = false;
  if (server_fd_ >= 0) {
    shutdown(server_fd_, SHUT_RDWR);
    close(server_fd_);
    server_fd_ = -1;
    unlink(socket_path_.c_str());
  }
  if (thread_.joinable()) thread_.join();
  for (int i = 0; i < MAX_CONSUMERS; ++i) {
    if (client_fds_[i] >= 0) close(client_fds_[i]);
  }
}

void DmaBufServer::RunLoop() {
  while (running_) {
    int nfds = 0;
    struct pollfd pfds[1 + MAX_CONSUMERS];

    // server fd
    pfds[nfds].fd = server_fd_;
    pfds[nfds].events = POLLIN;
    nfds++;

    // client fds
    int client_index[1 + MAX_CONSUMERS];  // pfd index -> consumer_id
    for (int i = 0; i < MAX_CONSUMERS; ++i) {
      if (client_fds_[i] >= 0) {
        pfds[nfds].fd = client_fds_[i];
        pfds[nfds].events = POLLIN;
        client_index[nfds] = i;
        nfds++;
      }
    }

    int ret = poll(pfds, nfds, -1);
    if (ret < 0) {
      if (errno == EINTR) continue;
      perror("DmaBufServer: poll");
      break;
    }
    if (!running_) break;

    for (int i = 0; i < nfds; ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;

      if (pfds[i].fd == server_fd_) {
        // ── New connection ──
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
          if (errno != EBADF && errno != EINVAL) perror("DmaBufServer: accept");
          continue;
        }

        // Find free consumer slot
        int consumer_id = -1;
        pthread_mutex_lock(&ctrl_->mtx);
        fprintf(stderr, "DmaBufServer: new connection, consumer_mask=0x%x w_idx=%u\n",
                ctrl_->consumer_mask, ctrl_->w_idx);
        for (int j = 0; j < MAX_CONSUMERS; ++j) {
          if (!(ctrl_->consumer_mask & (1u << j))) {
            consumer_id = j;
            break;
          }
        }
        pthread_mutex_unlock(&ctrl_->mtx);

        if (consumer_id < 0) {
          fprintf(stderr, "DmaBufServer: all consumer slots full, rejecting\n");
          close(client_fd);
          continue;
        }

        // Send dma-buf fds + handshake
        HandshakeMsg hs;
        hs.consumer_id = static_cast<uint32_t>(consumer_id);
        hs.frame_size = static_cast<uint32_t>(frame_size_);
        int send_err = SendFdsWithMsg(client_fd, dma_buf_fds_.data(),
                                      (int)dma_buf_fds_.size(), hs);
        if (send_err < 0) {
          fprintf(stderr, "DmaBufServer: sendmsg failed: %s\n",
                  strerror(-send_err));
          close(client_fd);
          continue;
        }

        // Receive registration with timeout
        struct timeval tv = {5, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        RegistrationMsg reg;
        ssize_t n = recv(client_fd, &reg, sizeof(reg), 0);
        if (n != sizeof(reg)) {
          fprintf(stderr, "DmaBufServer: recv registration failed\n");
          close(client_fd);
          continue;
        }

        // Register in SHM
        pthread_mutex_lock(&ctrl_->mtx);
        ctrl_->r_idx[consumer_id] = ctrl_->w_idx;
        ctrl_->consumer_flags[consumer_id] = reg.flags;
        ctrl_->consumer_mask |= (1u << consumer_id);
        pthread_mutex_unlock(&ctrl_->mtx);

        client_fds_[consumer_id] = client_fd;
        fprintf(stderr, "DmaBufServer: consumer %d connected (flags=0x%x)\n",
                consumer_id, reg.flags);

      } else {
        // ── Client event (disconnect) ──
        int consumer_id = client_index[i];
        char dummy;
        ssize_t n = recv(pfds[i].fd, &dummy, 1, MSG_PEEK);
        if (n <= 0) {
          fprintf(stderr, "DmaBufServer: consumer %d disconnected\n", consumer_id);
          pthread_mutex_lock(&ctrl_->mtx);
          ctrl_->consumer_mask &= ~(1u << consumer_id);
          ctrl_->r_idx[consumer_id] = ctrl_->w_idx;
          pthread_mutex_unlock(&ctrl_->mtx);
          close(client_fds_[consumer_id]);
          client_fds_[consumer_id] = -1;
        }
      }
    }
  }
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
