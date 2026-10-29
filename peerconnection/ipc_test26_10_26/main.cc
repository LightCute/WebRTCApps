// main.cc — 驱动多客户端 UnixSocketServer，演示观察者接口（回显 + quit）。
#include <cstdio>
#include <string>

#include "unix_socket_server.h"

// 演示观察者：收到消息回显到同一个客户端；收到 quit 请求退出。
class EchoObserver : public UnixSocketObserver {
 public:
  explicit EchoObserver(UnixSocketServer* server) : server_(server) {}

  void OnClientConnected(int client_id) override {
    printf("[demo] client %d connected\n", client_id);
  }

  void OnLineReceived(int client_id, const std::string& line) override {
    if (line == "quit") {
      server_->SendToClient(client_id, "bye");
      server_->RequestShutdown();
    } else {
      server_->SendToClient(client_id, line);
      printf("[demo] recive from client %d : %s\n", client_id, line.c_str());
    }
  }

  void OnClientDisconnected(int client_id) override {
    printf("[demo] client %d disconnected\n", client_id);
  }

 private:
  UnixSocketServer* server_;
};

int main(int argc, char* argv[]) {
  std::string path = (argc > 1) ? argv[1] : "/tmp/ipc_test.sock";

  UnixSocketServer server(path, 16);
  EchoObserver echo(&server);
  server.AddObserver(&echo);

  server.Start();
  printf("Unix socket server listening on %s\n", path.c_str());
  printf("Connect with: socat - UNIX-CONNECT:%s\n", path.c_str());
  printf("Send a line to echo it back; send 'quit' to stop the server.\n");

  server.Wait();  // 阻塞直到收到 quit

  printf("Server stopped.\n");
  return 0;
}
