// dma_buf_server.h — multi-client Unix socket server for dma-buf fd handoff
#ifndef APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_DMA_BUF_SERVER_H_
#define APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_DMA_BUF_SERVER_H_

#include <atomic>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

#include "apps/peerconnection/client_arm64/video_frame_shm_ctrl.h"

class DmaBufServer {
 public:
  DmaBufServer();
  ~DmaBufServer();

  // Start the poll-based accept/IO loop.
  // fds: dma-buf file descriptors for the pool slots
  // num_fds: number of slots (= RING_BUFFER_CNT)
  // frame_size: bytes per dma-buf slot
  // ctrl: pointer to SHM control block (producer-owned)
  int Start(const std::string& socket_path, const int* fds, int num_fds,
            size_t frame_size, ShmMultiCtrlBlock* ctrl);

  void Stop();

 private:
  void RunLoop();

  std::string socket_path_;
  std::vector<int> dma_buf_fds_;
  size_t frame_size_ = 0;
  ShmMultiCtrlBlock* ctrl_ = nullptr;

  int server_fd_ = -1;
  int client_fds_[MAX_CONSUMERS];  // -1 = slot free
  std::thread thread_;
  std::atomic<bool> running_{false};
};

#endif  // APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_DMA_BUF_SERVER_H_
