// dma_buf_reader.cc — multi-consumer zero-copy frame reader via control SHM + dma-buf
#include "dma_buf_reader.h"

#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

DmaBufReader::DmaBufReader() {
  for (int i = 0; i < kNumSlots; ++i) {
    slots_[i].fd = -1;
    slots_[i].ptr = nullptr;
    slots_[i].size = 0;
  }
}

DmaBufReader::~DmaBufReader() {
  for (int i = 0; i < kNumSlots; ++i) {
    if (slots_[i].ptr && slots_[i].ptr != MAP_FAILED) {
      munmap(slots_[i].ptr, slots_[i].size);
    }
    if (slots_[i].fd >= 0) close(slots_[i].fd);
  }
  if (ctrl_) shmdt(ctrl_);
  if (sock_fd_ >= 0) close(sock_fd_);
}

int DmaBufReader::Init(const std::string& ctrl_shm_path, int ctrl_proj_id,
                       const std::string& socket_path, uint32_t flags) {
  flags_ = flags;

  // 1. Attach control block SHM
  key_t key = ftok(ctrl_shm_path.c_str(), ctrl_proj_id);
  if (key == -1) {
    perror("DmaBufReader: ftok");
    return -1;
  }
  shmid_ = shmget(key, SHM_MULTI_CTRL_BLOCK_SIZE, 0666);
  if (shmid_ == -1) {
    perror("DmaBufReader: shmget");
    return -1;
  }
  ctrl_ = static_cast<ShmMultiCtrlBlock*>(shmat(shmid_, nullptr, 0));
  if (ctrl_ == reinterpret_cast<void*>(-1)) {
    perror("DmaBufReader: shmat");
    ctrl_ = nullptr;
    return -1;
  }

  // 2. Connect to producer's Unix socket
  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("DmaBufReader: socket");
    return -1;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  bool connected = false;
  for (int retry = 0; retry < 30; ++retry) {
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    usleep(100000);
  }
  if (!connected) {
    perror("DmaBufReader: connect");
    close(sock_fd);
    return -1;
  }

  // 3. Receive dma-buf fds + HandshakeMsg via SCM_RIGHTS
  HandshakeMsg hs;
  memset(&hs, 0, sizeof(hs));

  struct iovec iov = { .iov_base = &hs, .iov_len = sizeof(hs) };
  size_t cmsg_size = CMSG_SPACE(kNumSlots * sizeof(int));
  char* cmsg_buf = new char[cmsg_size];
  memset(cmsg_buf, 0, cmsg_size);

  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = cmsg_size;

  if (recvmsg(sock_fd, &msg, 0) < 0) {
    perror("DmaBufReader: recvmsg");
    delete[] cmsg_buf;
    close(sock_fd);
    return -1;
  }

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
      cmsg->cmsg_type != SCM_RIGHTS) {
    fprintf(stderr, "DmaBufReader: no SCM_RIGHTS received\n");
    delete[] cmsg_buf;
    close(sock_fd);
    return -1;
  }

  consumer_id_ = static_cast<int>(hs.consumer_id);
  size_t frame_size = hs.frame_size;

  int* fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
  int num_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);

  for (int i = 0; i < num_fds && i < kNumSlots; ++i) {
    int dup_fd = dup(fds[i]);
    if (dup_fd < 0) continue;

    void* ptr = mmap(nullptr, frame_size, PROT_READ, MAP_SHARED, dup_fd, 0);
    if (ptr == MAP_FAILED) {
      perror("DmaBufReader: mmap");
      close(dup_fd);
      continue;
    }
    slots_[i].fd = dup_fd;
    slots_[i].ptr = ptr;
    slots_[i].size = frame_size;
  }
  delete[] cmsg_buf;

  // 4. Send registration message
  RegistrationMsg reg;
  reg.flags = flags_;
  if (send(sock_fd, &reg, sizeof(reg), 0) != sizeof(reg)) {
    perror("DmaBufReader: send registration");
    close(sock_fd);
    return -1;
  }

  // 5. Initialize local read cursor from SHM
  r_idx_ = ctrl_->w_idx;

  sock_fd_ = sock_fd;  // Keep socket open — producer detects disconnect via close

  fprintf(stderr, "DmaBufReader: consumer_id=%d, %d dma-buf fds, frame_size=%zu\n",
          consumer_id_, num_fds, frame_size);
  inited_ = true;
  return 0;
}

bool DmaBufReader::ReadFrame(VideoFrameHead& out_head,
                             const uint8_t*& out_data) {
  if (!inited_ || !ctrl_) return false;

  // Wait for new frame with timeout
  pthread_mutex_lock(&ctrl_->mtx);
  while (r_idx_ == ctrl_->w_idx) {
    if (stop_requested_) {
      pthread_mutex_unlock(&ctrl_->mtx);
      return false;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000000;
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000;
    }
    int ret = pthread_cond_timedwait(&ctrl_->cv_can_read, &ctrl_->mtx, &ts);
    if (ret == ETIMEDOUT) {
      pthread_mutex_unlock(&ctrl_->mtx);
      return false;
    }
    if (ret != 0) {
      pthread_mutex_unlock(&ctrl_->mtx);
      return false;
    }
  }

  uint32_t slot = r_idx_;

  // Advance read cursor and release mutex BEFORE touching frame data.
  // Seqlock protects the data read — holding mutex during seqlock spin
  // would deadlock with the producer (which needs mutex to update metadata).
  r_idx_ = (r_idx_ + 1) % RING_BUFFER_CNT;
  ctrl_->r_idx[consumer_id_] = r_idx_;
  pthread_mutex_unlock(&ctrl_->mtx);

  // Seqlock-protected read (no mutex held — producer may be writing concurrently)
  uint32_t seq0 = 0, seq1 = 0;
  int retries = 0;
  do {
    seq0 = __atomic_load_n(&ctrl_->seq[slot], __ATOMIC_ACQUIRE);
    if (seq0 & 1) {  // producer writing, spin briefly
      if (++retries > 100) return false;  // producer stuck, give up
      usleep(50);
      continue;
    }
    out_head = ctrl_->ring[slot].head;
    out_data = static_cast<const uint8_t*>(slots_[slot].ptr);
    __sync_synchronize();  // fence: data read before second seq read
    seq1 = __atomic_load_n(&ctrl_->seq[slot], __ATOMIC_ACQUIRE);
  } while (seq0 != seq1);

  return true;
}

void DmaBufReader::SkipToLatest() {
  if (!inited_ || !ctrl_) return;
  pthread_mutex_lock(&ctrl_->mtx);
  r_idx_ = ctrl_->w_idx;
  ctrl_->r_idx[consumer_id_] = r_idx_;
  pthread_mutex_unlock(&ctrl_->mtx);
}

void DmaBufReader::RequestStop() {
  stop_requested_ = true;
}
