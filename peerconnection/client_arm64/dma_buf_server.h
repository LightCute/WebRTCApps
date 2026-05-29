// dma_buf_server.h — Unix socket server to pass dma-buf fds to consumer
#ifndef APPS_PEERCONNECTION_VIDEO_RGA_MPP_SHARED_DMA_BUF_SERVER_H_
#define APPS_PEERCONNECTION_VIDEO_RGA_MPP_SHARED_DMA_BUF_SERVER_H_

#include <atomic>
#include <string>
#include <thread>

// Sends dma-buf fds to multiple consumers via Unix domain socket.
// Each client that connects receives the full set of fds via SCM_RIGHTS.
// accept() blocks with zero CPU; late-joining clients are served automatically.
class DmaBufServer {
 public:
  DmaBufServer() = default;
  ~DmaBufServer();

  // Start server thread. Returns 0 on success.
  int Start(const std::string& socket_path, const int* fds, int num_fds);

  // Shutdown: unblock accept(), join thread, cleanup.
  void Stop();

 private:
  std::thread thread_;
  int server_fd_ = -1;
};

#endif
