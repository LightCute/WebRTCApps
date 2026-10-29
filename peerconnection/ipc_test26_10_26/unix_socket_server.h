// unix_socket_server.h
// 多客户端 Unix domain socket 服务器 + 观察者接口。
// 精简自 apps/client_x64_linux/unix_socket_server.h —— 保留 epoll 事件循环 +
// 行分帧 + 线程生命周期，剥离 JSON/引擎/rtc_base 依赖，供模块单独测试。
#ifndef APPS_PEERCONNECTION_IPC_TEST_UNIX_SOCKET_SERVER_H_
#define APPS_PEERCONNECTION_IPC_TEST_UNIX_SOCKET_SERVER_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// 观察者接口：UnixSocketServer 把「连接 / 收到完整一帧 / 断开」三类事件通知上层。
// 重要契约：所有回调都在 io 线程同步执行，观察者必须快速返回，勿做阻塞/重活；
// 需要重活请自行投递到上层线程/队列（对齐 EngineObserver 的注释）。
class UnixSocketObserver {
 public:
  virtual ~UnixSocketObserver() = default;
  virtual void OnClientConnected(int client_id) = 0;
  virtual void OnLineReceived(int client_id, const std::string& line) = 0;
  virtual void OnClientDisconnected(int client_id) = 0;
};

class UnixSocketServer {
 public:
  explicit UnixSocketServer(const std::string& socket_path, int max_clients = 16);
  ~UnixSocketServer();

  void Start();
  void Stop();            // 发退出信号 + join，阻塞等待 io 线程收尾
  void RequestShutdown(); // 只发信号不 join，供观察者在 io 线程内安全调用
  void Wait();            // 阻塞直到收到退出信号

  // 发送到指定客户端（client_id 由连接时分配）。
  void SendToClient(int client_id, const std::string& data);
  void Broadcast(const std::string& data);

  // 观察者注册（建议在 Start() 之前调用，避免与 io 线程并发）。
  void AddObserver(UnixSocketObserver* observer);
  void RemoveObserver(UnixSocketObserver* observer);

 private:
  struct Client {
    int id = -1;
    int fd = -1;
    std::string line_buf;    // 每客户端独立的接收缓冲
    bool discarding = false; // 超长丢弃模式：正在跳过当前行直到 '\n'
  };

  void IoLoop();
  void SendLine(int fd, const std::string& line);

  void NotifyConnected(int client_id);
  void NotifyLine(int client_id, const std::string& line);
  void NotifyDisconnected(int client_id);

  std::string socket_path_;
  int max_clients_;
  int listen_fd_ = -1;
  int next_client_id_ = 0;

  std::unordered_map<int, Client> clients_;  // key: fd（epoll 返回 fd，据此查）
  std::unordered_map<int, int> id_to_fd_;    // key: client_id（回复时路由）

  std::vector<UnixSocketObserver*> observers_;

  std::thread io_thread_;
  std::mutex mutex_;
  std::condition_variable shutdown_cv_;
  std::atomic<bool> running_{false};
};

#endif  // APPS_PEERCONNECTION_IPC_TEST_UNIX_SOCKET_SERVER_H_
