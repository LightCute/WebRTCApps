// dma_buf_server.h — Unix socket server to pass dma-buf fds to consumer
#ifndef APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_DMA_BUF_SERVER_H_
#define APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_DMA_BUF_SERVER_H_

#include <string>

// Sends dma-buf fds to a single consumer via Unix domain socket.
// One-shot: waits for connect, sends fds, thread exits.
class DmaBufServer {
 public:
  DmaBufServer() = default;
  ~DmaBufServer();

  // Start server thread. Returns 0 on success.
  int Start(const std::string& socket_path, const int* fds, int num_fds);

  // Join server thread.
  void Stop();

};

#endif  // APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_DMA_BUF_SERVER_H_
