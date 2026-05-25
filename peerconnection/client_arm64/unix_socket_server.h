// unix_socket_server.h
#ifndef APPS_PEERCONNECTION_CLIENT_UNIX_SOCKET_SERVER_H_
#define APPS_PEERCONNECTION_CLIENT_UNIX_SOCKET_SERVER_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "apps/peerconnection/client_arm64/engine_controller.h"

class EngineController;

class UnixSocketServer : public EngineObserver {
 public:
  UnixSocketServer(const std::string& socket_path, EngineController* engine);
  ~UnixSocketServer();

  void Start();
  void Stop();
  void Wait();                       // Block until shutdown
  void SendToClient(const std::string& json);

  // EngineObserver
  void OnEngineEvent(const std::string& json) override;

 private:
  void IoLoop();
  void HandleCommand(const std::string& raw_json, int client_fd);
  void SendJson(int fd, const std::string& json);
  void SendResponse(int id, bool ok, const std::string& error = "");

  std::string socket_path_;
  EngineController* engine_;             // non-owning
  int listen_fd_ = -1;
  int client_fd_ = -1;

  std::thread io_thread_;
  std::mutex mutex_;
  std::condition_variable shutdown_cv_;
  std::atomic<bool> running_{false};
};

#endif
