// pipe_transport_win.h — Windows Named Pipe implementation of IPipeTransport.
//
// Uses overlapped I/O + a worker thread to read lines from a named pipe.
// Compatible with QLocalSocket on the Qt side (Qt uses named pipes on Windows).
#ifndef APPS_PLATFORM_WINDOWS_PIPE_TRANSPORT_WIN_H_
#define APPS_PLATFORM_WINDOWS_PIPE_TRANSPORT_WIN_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

#include "apps/webrtc_engine/pipe_transport_interface.h"

class NamedPipeTransport : public IPipeTransport {
 public:
  explicit NamedPipeTransport(const std::string& pipe_name);
  ~NamedPipeTransport() override;

  bool Start(const std::string& endpoint) override;
  void Run() override;
  void Send(const std::string& data) override;
  void Stop() override;

 private:
  void IoLoop();

  std::string pipe_path_;   // "\\.\pipe\webrtc_ctrl"
  HANDLE hPipe_ = INVALID_HANDLE_VALUE;
  std::thread io_thread_;
  std::mutex mutex_;
  std::condition_variable shutdown_cv_;
  std::atomic<bool> running_{false};
  bool connected_ = false;
};

#endif  // APPS_PLATFORM_WINDOWS_PIPE_TRANSPORT_WIN_H_
