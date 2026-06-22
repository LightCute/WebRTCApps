// pipe_transport_win.cc
#include "apps/platform_windows/pipe_transport_win.h"

#include <cstring>

#include "rtc_base/logging.h"

// Pipe name prefix — QLocalSocket on Windows uses this format.
static std::string MakePipePath(const std::string& endpoint) {
  // Linux: /tmp/webrtc_runtime/webrtc_ctrl.sock
  // Windows: \\.\pipe\webrtc_ctrl
  // Extract just the filename part from the full path.
  auto pos = endpoint.rfind('/');
  std::string name = (pos != std::string::npos) ? endpoint.substr(pos + 1) : endpoint;
  return "\\\\.\\pipe\\" + name;
}

NamedPipeTransport::NamedPipeTransport(const std::string& pipe_name) {
  pipe_path_ = MakePipePath(pipe_name);
}

NamedPipeTransport::~NamedPipeTransport() { Stop(); }

bool NamedPipeTransport::Start(const std::string& /*endpoint*/) {
  running_ = true;
  io_thread_ = std::thread(&NamedPipeTransport::IoLoop, this);
  return true;
}

void NamedPipeTransport::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  shutdown_cv_.wait(lock, [this] { return !running_; });
}

void NamedPipeTransport::Send(const std::string& data) {
  if (!connected_) return;
  std::string line = data + "\n";
  DWORD written;
  WriteFile(hPipe_, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}

void NamedPipeTransport::Stop() {
  running_ = false;
  shutdown_cv_.notify_all();
  if (connected_) {
    DisconnectNamedPipe(hPipe_);
    connected_ = false;
  }
  if (hPipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(hPipe_);
    hPipe_ = INVALID_HANDLE_VALUE;
  }
  if (io_thread_.joinable()) io_thread_.join();
}

void NamedPipeTransport::IoLoop() {
  while (running_) {
    // Create a new pipe instance for each client connection.
    hPipe_ = CreateNamedPipeA(
        pipe_path_.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,               // max instances
        4096,            // output buffer
        4096,            // input buffer
        0,               // default timeout
        nullptr);        // default security

    if (hPipe_ == INVALID_HANDLE_VALUE) {
      RTC_LOG(LS_ERROR) << "CreateNamedPipe failed: " << GetLastError();
      break;
    }

    // Block until client connects.
    if (ConnectNamedPipe(hPipe_, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
      connected_ = true;
      if (on_connected) on_connected();
    } else {
      CloseHandle(hPipe_);
      hPipe_ = INVALID_HANDLE_VALUE;
      continue;
    }

    // Read loop — process lines from the connected client.
    char buf[4096];
    std::string line_buf;
    while (running_ && connected_) {
      DWORD nread = 0;
      BOOL ok = ReadFile(hPipe_, buf, sizeof(buf) - 1, &nread, nullptr);
      if (!ok || nread == 0) {
        // Client disconnected.
        break;
      }
      buf[nread] = '\0';
      line_buf.append(buf, nread);

      size_t pos;
      while ((pos = line_buf.find('\n')) != std::string::npos) {
        std::string line = line_buf.substr(0, pos);
        line_buf.erase(0, pos + 1);
        if (!line.empty() && on_message)
          on_message(line);
      }
    }

    // Client gone — clean up for next connection.
    connected_ = false;
    if (on_disconnected) on_disconnected();
    DisconnectNamedPipe(hPipe_);
    CloseHandle(hPipe_);
    hPipe_ = INVALID_HANDLE_VALUE;
  }
}
