// unix_socket_server.cc
// 多客户端实现：每个客户端独立接收缓冲，epoll 同时监听 listen_fd + 所有客户端 fd，
// 事件通过 UnixSocketObserver 通知上层。
#include "unix_socket_server.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// 单次 read() 的读取块大小（字节）。
// 注意：这不是消息长度上限 —— 完整的一行由 line_buf（std::string）累积到 '\n'，
// 不受此值限制。命令是短行，1KB 足够；想要更高吞吐可调大，栈占用可忽略。
constexpr int kReadBufSize = 1024;

// 单个「未结束的行」的最大字节数。SDP/ICE/聊天都是短行，8 KB 已放大约 40 倍。
// 超过则进入丢弃模式（见 IoLoop），防止无 '\n' 的数据让 line_buf 无限增长。
constexpr size_t kMaxLineBytes = 8 * 1024;

UnixSocketServer::UnixSocketServer(const std::string& socket_path, int max_clients)
    : socket_path_(socket_path), max_clients_(max_clients) {}

UnixSocketServer::~UnixSocketServer() { Stop(); }

void UnixSocketServer::Start() {
  running_ = true;
  io_thread_ = std::thread(&UnixSocketServer::IoLoop, this);
}

void UnixSocketServer::Stop() {
  RequestShutdown();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
}

void UnixSocketServer::RequestShutdown() {
  running_ = false;
  shutdown_cv_.notify_all();
}

void UnixSocketServer::Wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  shutdown_cv_.wait(lock, [this] { return !running_; });//false 继续睡觉 ； true 退出
}

void UnixSocketServer::AddObserver(UnixSocketObserver* observer) {
  if (!observer) return;
  observers_.push_back(observer);
}

void UnixSocketServer::RemoveObserver(UnixSocketObserver* observer) {
  for (auto it = observers_.begin(); it != observers_.end(); ++it) {
    if (*it == observer) {
      observers_.erase(it);
      return;
    }
  }
}

void UnixSocketServer::SendToClient(int client_id, const std::string& data) {
  auto it = id_to_fd_.find(client_id);   // id → fd 路由
  if (it == id_to_fd_.end()) return;
  SendLine(it->second, data);
}

void UnixSocketServer::Broadcast(const std::string& data) {
  for (const auto& kv : clients_) {
    SendLine(kv.second.fd, data);
  }
}

void UnixSocketServer::SendLine(int fd, const std::string& line) {
  std::string msg = line + "\n";
  while (!msg.empty()) {
    ssize_t n = write(fd, msg.data(), msg.size());
    if (n < 0) {
      if (errno == EINTR) continue;  // 被信号打断，重试
      break;                        // 其他错误（如对端已关），放弃
    }
    msg.erase(0, n);  // 消费已写出的部分
  }
}

void UnixSocketServer::NotifyConnected(int client_id) {
  for (auto* obs : observers_) obs->OnClientConnected(client_id);
}

void UnixSocketServer::NotifyLine(int client_id, const std::string& line) {
  for (auto* obs : observers_) obs->OnLineReceived(client_id, line);
}

void UnixSocketServer::NotifyDisconnected(int client_id) {
  for (auto* obs : observers_) obs->OnClientDisconnected(client_id);
}

void UnixSocketServer::IoLoop() {
  unlink(socket_path_.c_str());  // 删除上一次运行残留的 socket 文件

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);  // 创建 socket，返回 fd
  if (listen_fd_ < 0) {
    fprintf(stderr, "[ERR] Failed to create Unix socket: %s\n", strerror(errno));
    return;
  }

  struct sockaddr_un addr = {};  // 全 0 初始化
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);  // 对 addr 做配置，设置地址
  if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {  // 地址绑定到监听 fd
    fprintf(stderr, "[ERR] Failed to bind Unix socket: %s\n", strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  if (listen(listen_fd_, max_clients_) < 0) {  // backlog = 最大客户端数
    fprintf(stderr, "[ERR] Failed to listen on Unix socket: %s\n",
            strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return;
  }

  int epfd = epoll_create1(0);
  if (epfd < 0) {
    fprintf(stderr, "[ERR] Failed to create epoll: %s\n", strerror(errno));
    return;
  }

  struct epoll_event ev = {};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd_;
  epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd_, &ev);  // 把监听 fd 放进 epoll 表

  char buf[kReadBufSize];
  std::vector<epoll_event> events(max_clients_ + 1);  // +1 给 listen_fd

  while (running_) {
    int nfds = epoll_wait(epfd, events.data(), (int)events.size(), 100);

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;

      if (fd == listen_fd_) {  // 有新的客户端发起连接
        int cfd = accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) continue;
        if ((int)clients_.size() >= max_clients_) {  // 超上限，拒绝
          fprintf(stderr, "[WARN] Too many clients, rejecting fd=%d\n", cfd);
          close(cfd);
          continue;
        }
        int id = next_client_id_++;  // 分配稳定 client_id
        clients_[cfd].id = id;
        clients_[cfd].fd = cfd;
        id_to_fd_[id] = cfd;
        struct epoll_event cev = {};
        cev.events = EPOLLIN | EPOLLRDHUP;
        cev.data.fd = cfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);  // 把新客户端 fd 放进 epoll 表
        fprintf(stderr, "[INFO] client %d connected (fd=%d)\n", id, cfd);
        NotifyConnected(id);
      } else {  // 是客户端 fd，直接读取消息
        auto it = clients_.find(fd);
        if (it == clients_.end()) continue;  // 防御，不该发生
        Client& client = it->second;         // 每个客户端独立的接收缓冲

        ssize_t n = read(fd, buf, sizeof(buf) - 1);  // 读取内容
        if (n < 0 && errno == EINTR) {
          continue;  // 被信号打断，继续等下轮，不视为断开
        }
        if (n <= 0) {  // 客户端关闭或出现错误
          int id = client.id;
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
          close(fd);
          clients_.erase(it);   // 从两张表里删除
          id_to_fd_.erase(id);
          fprintf(stderr, "[INFO] client %d disconnected\n", id);
          NotifyDisconnected(id);
          continue;
        }

        if (client.discarding) {
          // 丢弃模式：只找 '\n' 重新对齐，不再累积数据
          std::string_view chunk(buf, n);
          size_t nl = chunk.find('\n');
          if (nl == std::string_view::npos) continue;  // 仍没到换行，继续丢弃
          client.discarding = false;
          client.line_buf.append(chunk.substr(nl + 1));  // '\n' 之后重新入缓冲
        } else {
          client.line_buf.append(buf, n);  // 正常累积
        }

        // 分帧（空行也交付，语义交给上层）
        size_t pos;
        while ((pos = client.line_buf.find('\n')) != std::string::npos) {
          std::string line = client.line_buf.substr(0, pos);
          client.line_buf.erase(0, pos + 1);
          if (!line.empty() && line.back() == '\r') {  // 归一化 CRLF → LF
            line.pop_back();
          }
          if (line.size() > kMaxLineBytes) {
            // 已完整的超长行：丢弃、告警，不交付上层
            fprintf(stderr,
                    "[WARN] client %d dropped over-long line (%zu bytes)\n",
                    client.id, line.size());
            continue;
          }
          NotifyLine(client.id, line);
        }

        // 单行超长保护：进入丢弃模式，而不是断连
        if (!client.discarding && client.line_buf.size() >= kMaxLineBytes) {
          size_t dropped = client.line_buf.size();
          client.line_buf.clear();
          client.discarding = true;
          fprintf(stderr,
                  "[WARN] client %d line too long (%zu bytes), discarding until newline\n",
                  client.id, dropped);
        }
      }
    }
  }

  // 收尾：关闭所有客户端 fd（统一在 io 线程收尾，避免 Stop 并发 close 竞态）
  for (auto& kv : clients_) {
    close(kv.second.fd);
  }
  clients_.clear();
  id_to_fd_.clear();
  close(epfd);
  close(listen_fd_);
  listen_fd_ = -1;
  unlink(socket_path_.c_str());
}
