// pipe_transport_unix.h — Unix domain socket implementation of IPipeTransport.
#ifndef APPS_CLIENT_X64_LINUX_PIPE_TRANSPORT_UNIX_H_
#define APPS_CLIENT_X64_LINUX_PIPE_TRANSPORT_UNIX_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "apps/webrtc_engine/pipe_transport_interface.h"

class UnixSocketTransport : public IPipeTransport {
 public:
  explicit UnixSocketTransport(const std::string& socket_path);
  ~UnixSocketTransport() override;

  bool Start(const std::string& endpoint) override;
  void Run() override;
  void Send(const std::string& data) override;
  void Stop() override;

 private:
  void IoLoop();
  std::string socket_path_;
  std::thread io_thread_;
  std::mutex mutex_;
  std::condition_variable shutdown_cv_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  int client_fd_ = -1;
};

#endif  // APPS_CLIENT_X64_LINUX_PIPE_TRANSPORT_UNIX_H_
