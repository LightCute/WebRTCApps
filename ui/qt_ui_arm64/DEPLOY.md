# 陪护机器人 — arm64 部署指南

## 目录结构 (机器人端)

```
/home/elf/webrtc_monitor/
├── ui_rk                    ← Qt UI 可执行文件
├── client_arm64             ← WebRTC daemon
├── start_ui.sh              ← 启动脚本
├── material-dark.qss        ← 样式表
├── models/                  ← AI 模型 + 推理二进制
│   ├── yolov5-coco80/
│   ├── yolov11-human-fall-detection/
│   └── yolov8-fire-smoke-detection/
├── whisper_asr/             ← 语音对话
├── reason/                  ← AI 推理程序
└── voice_chat/              ← 语音对话 Python 脚本
```

## 部署步骤

```bash
# 1. 创建目录
ssh elf@<robot_ip> "mkdir -p /home/elf/webrtc_monitor/models"

# 2. 拷贝可执行文件
scp ui_rk client_arm64 elf@<robot_ip>:/home/elf/webrtc_monitor/

# 3. 拷贝资源和脚本
scp start_ui.sh material-dark.qss elf@<robot_ip>:/home/elf/webrtc_monitor/
scp -r models/* elf@<robot_ip>:/home/elf/webrtc_monitor/models/
scp -r whisper_asr reason voice_chat elf@<robot_ip>:/home/elf/webrtc_monitor/

# 4. 赋权
ssh elf@<robot_ip> "chmod +x /home/elf/webrtc_monitor/ui_rk /home/elf/webrtc_monitor/client_arm64 /home/elf/webrtc_monitor/start_ui.sh"

# 5. 安装桌面图标
scp webrtc-robot.desktop elf@<robot_ip>:/home/elf/Desktop/
# chmod +x 让桌面环境识别为可信任的启动器
ssh elf@<robot_ip> "chmod +x /home/elf/Desktop/webrtc-robot.desktop"
# 如果 chmod +x 后仍不显示图标，用 gio 标记信任（需要在桌面会话中运行）:
# ssh elf@<robot_ip> "dbus-launch gio set /home/elf/Desktop/webrtc-robot.desktop metadata::trusted true"

# 6. 串口权限
ssh elf@<robot_ip> "sudo usermod -a -G dialout elf"
# 重启后生效
```

## 进程依赖关系

```
ui_rk (Qt UI)
  │
  ├─ QProcess → client_arm64 (WebRTC 引擎)
  │     ├─ 创建 SHM (/tmp/webrtc_runtime/shm_video_buf_*)
  │     ├─ 创建 Unix Socket (/tmp/webrtc_runtime/webrtc_ctrl.sock)
  │     └─ 创建 DMA-BUF Socket (/tmp/webrtc_runtime/shm_video_buf_*_socket)
  │
  ├─ QProcess → reason / fall_detect / fire_detect (AI 推理)
  │     └─ 读取 SHM 帧 → 输出到 /tmp/webrtc_runtime/ai_detections.sock
  │
  ├─ QProcess → voice_chat Python (语音对话)
  │
  ├─ SerialWorker → /dev/ttyS9 (MCU 串口)
  │
  └─ AiReceiver → Unix Socket (/tmp/webrtc_runtime/ai_detections.sock)
```

## 运行时文件

所有运行时 IPC 文件在 `/tmp/webrtc_runtime/`：

| 文件 | 用途 |
|------|------|
| `webrtc_ctrl.sock` | UI ↔ Daemon 控制通道 |
| `shm_video_buf_local` | 本地摄像头 SHM 帧 |
| `shm_video_buf_remote` | 远端视频 SHM 帧 |
| `shm_video_buf_*_socket` | DMA-BUF fd 传输 |
| `ai_detections.sock` | AI 推理结果 |
| `voice_chat.sock` | 语音对话 |
| `daemon_*.log` | Daemon 日志 |
