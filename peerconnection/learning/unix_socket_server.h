#ifndef APPS_PEERCONNECTION_DC_CALL_UNIX_SOCKET_SERVER_H_
#define APPS_PEERCONNECTION_DC_CALL_UNIX_SOCKET_SERVER_H_

#include <string>

// Simple blocking Unix domain socket server for dc_call.
// Binds to a path, accepts a single client, provides line-based I/O.
class UnixSocketServer {
 public:
  explicit UnixSocketServer(const std::string& socket_path);
  ~UnixSocketServer();

  UnixSocketServer(const UnixSocketServer&) = delete;
  UnixSocketServer& operator=(const UnixSocketServer&) = delete;

  // Block until a client connects. Returns true on success.
  bool WaitForClient();

  // Read a line from the client (blocking). Returns empty string on disconnect.
  std::string ReadLine();

  // Send a line to the client (appends newline).
  void WriteLine(const std::string& line);

  // Send raw string (no newline appended).
  void Write(const std::string& data);

  bool IsConnected() const { return client_fd_ >= 0; }

  void Disconnect();

 private:
  std::string socket_path_;
  int listen_fd_ = -1;
  int client_fd_ = -1;
  std::string recv_buf_;  // buffered receive data
};

#endif  // APPS_PEERCONNECTION_DC_CALL_UNIX_SOCKET_SERVER_H_
