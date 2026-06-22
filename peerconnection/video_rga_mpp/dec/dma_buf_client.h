#ifndef APPS_PEERCONNECTION_VIDEO_RGA_MPP_DEC_DMA_BUF_CLIENT_H_
#define APPS_PEERCONNECTION_VIDEO_RGA_MPP_DEC_DMA_BUF_CLIENT_H_

#include <string>
#include <vector>

class DmaBufClient {
 public:
  DmaBufClient() = default;
  ~DmaBufClient();

  // Connect to producer's Unix socket and receive dma-buf fds.
  // Returns received fd list. Retries up to 3 seconds.
  std::vector<int> ReceiveFds(const std::string& socket_path, int expected_count);

 private:
  int sock_fd_ = -1;
};

#endif
