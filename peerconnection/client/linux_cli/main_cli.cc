/*
 *  Copyright 2026 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "apps/peerconnection/client/linux_cli/main_cli.h"

#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>
#include "api/video/i420_buffer.h"
#include "api/video/video_rotation.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"

// ──────────────────────────────────────────────────────────
// CliVideoRenderer 实现
// ──────────────────────────────────────────────────────────
CliMainWnd::CliVideoRenderer::CliVideoRenderer(const std::string& key_path, int proj_id) {
  shm_writer_ = std::make_unique<ShmVideoWriter>();
  if (!shm_writer_->init(key_path, proj_id)) {
    RTC_LOG(LS_ERROR) << "ShmVideoWriter init failed for " << key_path;
    shm_writer_.reset();
    return;
  }
  RTC_LOG(LS_INFO) << "共享内存写入端初始化完成: " << key_path;
}

CliMainWnd::CliVideoRenderer::~CliVideoRenderer() {
  shm_writer_.reset();
  RTC_LOG(LS_INFO) << "共享内存写入端已关闭";
}

void CliMainWnd::CliVideoRenderer::OnFrame(const webrtc::VideoFrame& frame) {
  if (!shm_writer_) return;

  auto buffer = frame.video_frame_buffer()->ToI420();

  if (frame.rotation() != webrtc::kVideoRotation_0) {
    buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
  }

  int w = buffer->width();
  int h = buffer->height();
  if (width_ != w || height_ != h) {
    width_ = w;
    height_ = h;
    RTC_LOG(LS_INFO) << "Video resolution: " << width_ << "x" << height_;
  }

  size_t y_size = static_cast<size_t>(w) * h;
  size_t uv_size = y_size / 4;
  size_t total = y_size + uv_size * 2;

  if (total > FRAME_MAX_SIZE) {
    RTC_LOG(LS_ERROR) << "Frame too large: " << total << " > " << FRAME_MAX_SIZE;
    return;
  }

  std::vector<uint8_t> i420_data(total);
  memcpy(&i420_data[0], buffer->DataY(), y_size);
  memcpy(&i420_data[y_size], buffer->DataU(), uv_size);
  memcpy(&i420_data[y_size + uv_size], buffer->DataV(), uv_size);

  VideoFrameHead head{};
  head.timestamp = frame.render_time_ms() * 1000;  // ms → µs
  head.frame_len = static_cast<uint32_t>(total);
  head.width = static_cast<uint16_t>(w);
  head.height = static_cast<uint16_t>(h);
  head.frame_type = 0;

  shm_writer_->write_frame(head, i420_data.data());
}

// ──────────────────────────────────────────────────────────
// CliMainWnd 构造/析构
// ──────────────────────────────────────────────────────────
CliMainWnd::CliMainWnd(const char* server, int port, bool autoconnect, bool autocall)
    : running_(false),
      server_(server),
      autoconnect_(autoconnect),
      autocall_(autocall),
      task_queue_(100)
      {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%d", port);
  port_ = buffer;
  //running_ = true;
  SwitchToConnectUI();
}

CliMainWnd::~CliMainWnd() {
  Stop();
}

// ──────────────────────────────────────────────────────────
// MainWindow 接口实现
// ──────────────────────────────────────────────────────────
void CliMainWnd::RegisterObserver(MainWndCallback* callback) {
  callback_ = callback;
}

bool CliMainWnd::IsWindow() {
  return running_;
}

void CliMainWnd::MessageBox(const char* caption, const char* text, bool is_error) {
  std::cout << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << (is_error ? "❌ [ERROR] " : "ℹ️  [INFO] ") << caption << std::endl;
  std::cout << "───────────────────────────────────────────────" << std::endl;
  std::cout << text << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << std::endl;
}

MainWindow::UI CliMainWnd::current_ui() {
  return current_ui_;
}

void CliMainWnd::PrintPrompt() {
  std::cout << "> " << std::flush;
}

void CliMainWnd::SwitchToConnectUI() {
  current_ui_ = CONNECT_TO_SERVER;
  std::cout << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << "          🌐 连接服务器界面" << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << "当前配置: " << server_ << ":" << port_ << std::endl;
  std::cout << std::endl;
  std::cout << "可用命令:" << std::endl;
  std::cout << "  connect           - 使用当前配置连接" << std::endl;
  std::cout << "  server <addr>     - 设置服务器地址" << std::endl;
  std::cout << "  port <num>        - 设置端口" << std::endl;
  std::cout << "  quit              - 退出程序" << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  PrintPrompt();

  if (autoconnect_) {
    std::cout << "(自动连接中...)" << std::endl;
    callback_->StartLogin(server_, atoi(port_.c_str()));
  }
}

void CliMainWnd::SwitchToPeerList(const Peers& peers) {
  current_ui_ = LIST_PEERS;
  current_peers_ = peers;

  std::cout << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << "          👥 在线 Peer 列表" << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << "ID\t\tName" << std::endl;
  std::cout << "───────────────────────────────────────────────" << std::endl;
  
  bool has_peers = false;
  for (const auto& peer : peers) {
    if (peer.first != -1) {
      std::cout << peer.first << "\t\t" << peer.second << std::endl;
      has_peers = true;
    }
  }
  
  if (!has_peers) {
    std::cout << "(暂无在线 peer)" << std::endl;
  }
  
  std::cout << "───────────────────────────────────────────────" << std::endl;
  std::cout << "可用命令:" << std::endl;
  std::cout << "  call <peer_id>    - 呼叫指定 peer" << std::endl;
  std::cout << "  disconnect        - 断开服务器" << std::endl;
  std::cout << "  quit              - 退出程序" << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  PrintPrompt();

  if (autocall_ && has_peers) {
    auto last_peer = peers.rbegin();
    if (last_peer->first != -1) {
      std::cout << "(自动呼叫 peer " << last_peer->first << ")" << std::endl;
      callback_->ConnectToPeer(last_peer->first);
    }
  }
}

void CliMainWnd::SwitchToStreamingUI() {
  current_ui_ = STREAMING;
  std::cout << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << "          🎥 视频通话中" << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  std::cout << "本地视频 SHM: " << LOCAL_SHM_KEY << " (id=" << LOCAL_SHM_ID << ")" << std::endl;
  std::cout << "远端视频 SHM: " << REMOTE_SHM_KEY << " (id=" << REMOTE_SHM_ID << ")" << std::endl;
  std::cout << std::endl;
  std::cout << "可用命令:" << std::endl;
  std::cout << "  hangup            - 挂断通话" << std::endl;
  std::cout << "  quit              - 退出程序" << std::endl;
  std::cout << "═══════════════════════════════════════════════" << std::endl;
  PrintPrompt();
}

void CliMainWnd::StartLocalRenderer(webrtc::VideoTrackInterface* local_video) {
  local_renderer_ = std::make_unique<CliVideoRenderer>(LOCAL_SHM_KEY, LOCAL_SHM_ID);
  local_video->AddOrUpdateSink(local_renderer_.get(), webrtc::VideoSinkWants());
}

void CliMainWnd::StopLocalRenderer() {
  local_renderer_.reset();
}

void CliMainWnd::StartRemoteRenderer(webrtc::VideoTrackInterface* remote_video) {
  remote_renderer_ = std::make_unique<CliVideoRenderer>(REMOTE_SHM_KEY, REMOTE_SHM_ID);
  remote_video->AddOrUpdateSink(remote_renderer_.get(), webrtc::VideoSinkWants());
}

void CliMainWnd::StopRemoteRenderer() {
  remote_renderer_.reset();
}


void CliMainWnd::QueueUIThreadCallback(int msg_id, void* data) {
  // 🔥 WebRTC回调直接投递到信令线程
  signaling_thread_->PostTask([this, msg_id, data]() {
    callback_->UIThreadCallback(msg_id, data);
  });
}

// ──────────────────────────────────────────────────────────
// CLI 事件循环
// ──────────────────────────────────────────────────────────
// ==================== 线程1：用户输入线程 ====================
void CliMainWnd::InputThreadFunc() {
    while (running_) {
        std::string input;
        // 阻塞读输入（独立线程，不影响信令）
        if (!std::getline(std::cin, input))
            break;

        // 🔥 线程安全投递：CLI命令在信令线程执行
        if (signaling_thread_) {
            signaling_thread_->PostTask([this, input]() {
                HandleUserInput(input);
            });
        }
    }
}



// ==================== 启动双线程 ====================
void CliMainWnd::Run() {
  if (running_) return;
  running_ = true;

  input_thread_ = std::thread(&CliMainWnd::InputThreadFunc, this);
}

void CliMainWnd::Stop() {
  if (!running_) return;
  running_ = false;


  // 等待线程退出
  if (input_thread_.joinable()) input_thread_.join();

  // 停止 WebRTC
  if (webrtc::Thread::Current()) webrtc::Thread::Current()->Quit();
}



void CliMainWnd::HandleUserInput(const std::string& input) {
  if (input.empty()) {
    PrintPrompt();
    return;
  }

  std::istringstream iss(input);
  std::string cmd;
  iss >> cmd;

  if (cmd == "quit") {
    // 🔥 统一写法：关闭 WebRTC
    callback_->Close();
    Stop();
    return;
  }

  switch (current_ui_) {
    case CONNECT_TO_SERVER: {
      if (cmd == "connect") {
        int port = !port_.empty() ? atoi(port_.c_str()) : 0;
        callback_->StartLogin(server_, port);
      } else if (cmd == "server") {
        iss >> server_;
        std::cout << "服务器地址已设置为: " << server_ << std::endl;
        PrintPrompt();
      } else if (cmd == "port") {
        iss >> port_;
        std::cout << "端口已设置为: " << port_ << std::endl;
        PrintPrompt();
      } else {
        std::cout << "未知命令: " << cmd << std::endl;
        PrintPrompt();
      }
      break;
    }
    case LIST_PEERS: {
      if (cmd == "call") {
        int peer_id;
        if (iss >> peer_id) {
            callback_->ConnectToPeer(peer_id);
        } else {
          std::cout << "用法: call <peer_id>" << std::endl;
          PrintPrompt();
        }
      } else if (cmd == "disconnect") {
        callback_->DisconnectFromServer();
      } else {
        std::cout << "未知命令: " << cmd << std::endl;
        PrintPrompt();
      }
      break;
    }
    case STREAMING: {
      if (cmd == "hangup") {
        callback_->DisconnectFromServer();
      } else {
        std::cout << "未知命令: " << cmd << std::endl;
        PrintPrompt();
      }
      break;
    }
  }
}


