// unix_socket_server.h
#ifndef APPS_PEERCONNECTION_CLIENT_UNIX_SOCKET_SERVER_H_
#define APPS_PEERCONNECTION_CLIENT_UNIX_SOCKET_SERVER_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class WebRTCEngine;

class UnixSocketServer {
 public:
  UnixSocketServer(const std::string& socket_path, WebRTCEngine* engine);
  ~UnixSocketServer();

  void Start();
  void Stop();
  void Wait();                       // Block until shutdown
  void SendToClient(const std::string& json);

 private:
  void IoLoop();
  void HandleCommand(const std::string& raw_json, int client_fd);
  void SendJson(int fd, const std::string& json);
  void SendResponse(int id, bool ok, const std::string& error = "");

  std::string socket_path_;
  WebRTCEngine* engine_;             // non-owning
  int listen_fd_ = -1;
  int client_fd_ = -1;

  std::thread io_thread_;
  std::mutex mutex_;
  std::condition_variable shutdown_cv_;
  std::atomic<bool> running_{false};
};

#endif
