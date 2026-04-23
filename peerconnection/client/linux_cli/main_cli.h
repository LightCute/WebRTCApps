/*
 *  Copyright 2026 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef APPS_PEERCONNECTION_CLIENT_LINUX_CLI_MAIN_WND_H_
#define APPS_PEERCONNECTION_CLIENT_LINUX_CLI_MAIN_WND_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdio>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "apps/peerconnection/client/main_wnd.h"
#include "apps/peerconnection/client/peer_connection_client.h"
#include "apps/peerconnection/client/blocking_queue.h"
#include "rtc_base/buffer.h"
#include "rtc_base/thread.h"



class CliMainWnd : public MainWindow {
 public:
  CliMainWnd(const char* server, int port, bool autoconnect, bool autocall);
  ~CliMainWnd() override;

  // ──────────────────────────────────────────────────────────
  // MainWindow 纯虚函数实现
  // ──────────────────────────────────────────────────────────
  void RegisterObserver(MainWndCallback* callback) override;
  bool IsWindow() override;
  void MessageBox(const char* caption, const char* text, bool is_error) override;
  UI current_ui() override;
  void SwitchToConnectUI() override;
  void SwitchToPeerList(const Peers& peers) override;
  void SwitchToStreamingUI() override;
  void StartLocalRenderer(webrtc::VideoTrackInterface* local_video) override;
  void StopLocalRenderer() override;
  void StartRemoteRenderer(webrtc::VideoTrackInterface* remote_video) override;
  void StopRemoteRenderer() override;
  void QueueUIThreadCallback(int msg_id, void* data) override;

  // ──────────────────────────────────────────────────────────
  // CLI 特有方法
  // ──────────────────────────────────────────────────────────
  void Run();
  void Stop();
  void InputThreadFunc();    // 用户输入线程
  void set_signaling_thread(webrtc::Thread* thread) {
    signaling_thread_ = thread;
  }
 private:
  struct UIThreadCallbackData {
    int msg_id;
    void* data;
  };
  // ──────────────────────────────────────────────────────────
  // 内部类：CLI 视频渲染器（存 YUV 文件）
  // ──────────────────────────────────────────────────────────
  class CliVideoRenderer : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    explicit CliVideoRenderer(const std::string& output_path);
    ~CliVideoRenderer() override;
    void OnFrame(const webrtc::VideoFrame& frame) override;

   private:
    std::string output_path_;
    FILE* file_ = nullptr;
    int width_ = 0;
    int height_ = 0;
  };

  // ──────────────────────────────────────────────────────────
  // 内部辅助方法
  // ──────────────────────────────────────────────────────────
  void HandleUserInput(const std::string& input);
  void PrintPrompt();
  // ──────────────────────────────────────────────────────────
  // 成员变量
  // ──────────────────────────────────────────────────────────
  MainWndCallback* callback_ = nullptr;
  UI current_ui_ = CONNECT_TO_SERVER;
  Peers current_peers_;

  std::unique_ptr<CliVideoRenderer> local_renderer_;
  std::unique_ptr<CliVideoRenderer> remote_renderer_;

  // UI 回调队列
  struct CallbackData {
    int msg_id;
    void* data;
  };

  // 事件循环控制
  std::atomic<bool> running_;
  std::thread input_thread_;    // 线程1：监听用户输入
  //webrtc::Thread* rtc_thread_ = nullptr;

  // 初始参数
  std::string server_;
  std::string port_;
  bool autoconnect_;
  bool autocall_;

  BlockingQueue<UIThreadCallbackData> task_queue_;
  webrtc::Thread* signaling_thread_ = nullptr; 
};

#endif  // APPS_PEERCONNECTION_CLIENT_LINUX_CLI_MAIN_WND_H_