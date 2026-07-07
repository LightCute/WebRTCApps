# 居家陪护机器人 — Home Care Robot

> 基于 RK3588 + STM32 异构双芯片架构的 WebRTC 远程陪护机器人系统


---

## 系统概览

```
┌──────────────┐     公网信令服务器       ┌──────────────────────┐
│   PC 监护端   │◄───── WebRTC ──────►│    机器人端             │
│              │    P2P 音视频+控制      │                       │
│  qt_ui_x64   │                       │  qt_ui_arm64 (7寸屏)   │
│  client_x64  │                       │  client_arm64 (RK3588) │
│  键盘/鼠标控制│                       │  STM32 MCU (底盘/云台)  │
│  纯软件编解码 │                       │  RGA+MPP 硬件加速       │
│              │                       │  AI 推理 (NPU)         │
└──────────────┘                       └───────────────────────┘
```

监护者通过 PC 端发起 WebRTC 音视频通话，远程控制机器人底盘移动和云台视角。RK3588 NPU 运行 3 种 AI 视觉模型，实现行人追踪、跌倒检测、火灾烟雾告警。无人连接时机器人可独立运行语音对话陪聊。

---

## 核心功能

| 功能 | 说明 |
|------|------|
| **公网音视频通话** | WebRTC P2P，Host/STUN/TURN 三重网络保障，RTT 35ms，丢包率 0.2% |
| **远程底盘/云台控制** | WASD/IJKL 键盘 + 组合键 + 可调速，事件驱动，按下即发 |
| **AI 行人追踪** | YOLOv5 NPU 推理，最小框锁定目标，底盘自动跟随，舒适距离保持 |
| **跌倒/火灾监护** | YOLOv11 跌倒 + YOLOv8 火灾烟雾，在线热切换，自动告警推送 |
| **语音陪聊** | Whisper 本地语音识别 + 云端 LLM + 本地 TTS，多轮对话记忆 |

---

## 技术亮点

- **RK3588 硬件加速视频管线** — RGA 色彩转换 + MPP H.264 编解码 (<1ms/帧) + DMA-BUF 零拷贝
- **异构双芯片架构** — RK3588 负责音视频/AI/UI，自研 STM32 MCU 负责底盘实时控制
- **自定义 MCU 串口协议** — 0xAA 0x55 帧封装 + XOR 校验 + 300ms 看门狗安全停机
- **WebRTC 公网穿透** — HTTP 长轮询信令 + ICE 三重候选 + SCTP DataChannel

---

## 硬件组成

| 组件 | 型号/参数 |
|------|----------|
| 核心板 | ELF2 RK3588 (8nm, 4×A76+4×A55, NPU 6TOPS) |
| MCU | 自研 STM32H750 控制板 |
| 摄像头 | USB/V4L2, 640×480@30fps |
| 屏幕 | 7" LCD, 1024×600 |
| 底盘 | 四轮差速 + 二维舵机云台 |
| 音频 | NAU8822 麦克风 + 外接音箱 |
| 供电 | 2200mAh 锂电池 |

---

## 软件架构

```
RK3588 (Linux)
├── ui_rk (Qt5 主进程)
│   ├── QProcess → client_arm64 (WebRTC daemon)
│   ├── QProcess → AI 推理进程 ×3 (RKNN/NPU)
│   ├── QProcess → voice_chat.py (Whisper+LLM+TTS)
│   ├── SerialWorker → /dev/ttyS9 → STM32 MCU
│   └── AiReceiver ← /tmp/ai_detections.sock
│
STM32 MCU (裸机)
├── protocol.c  — 串口协议栈 + 看门狗
├── tb6612.c    — 电机驱动 (TB6612 双H桥)
└── servo.c     — 舵机云台 (50Hz PWM)
```

---

## 实测性能

| 指标 | 数值 |
|------|------|
| 视频分辨率/帧率 | 640×480 @30fps |
| RTT 往返延迟 | 35.0 ms |
| 丢包率 | 0.2% |
| MPP 编码耗时 | <1 ms/帧 |
| MPP 解码耗时 | <1 ms/帧 |
| AI 推理间隔 | 250 ms (NPU) |
| ICE 连接类型 | prflx (STUN 穿透) |
| 发送码率 | 1505 kbps |

---

## 目录结构

```
apps/
├── client_arm64_linux/     # 机器人端 WebRTC daemon (RGA+MPP硬件加速)
├── client_x64_linux/       # PC端 WebRTC daemon (纯软件编解码)
├── peerconnection/server/  # 公网信令服务器
├── webrtc_engine/          # WebRTC 核心引擎（共享代码）
├── ui/
│   ├── qt_ui_arm64/        # 机器人端 Qt5 UI (7寸屏)
│   └── qt_ui_x64/          # PC端 Qt5 UI (键盘控制)
└── platform_windows/       # Windows 平台适配 (待完善)
```

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [DESIGN_REPORT.md](ui/qt_ui_arm64/DESIGN_REPORT.md) | 竞赛设计报告全文 |
| [SYSTEM_ARCHITECTURE.md](ui/qt_ui_arm64/SYSTEM_ARCHITECTURE.md) | 系统架构文档 |
| [WEBRTC_DEEP_DIVE.md](ui/qt_ui_arm64/WEBRTC_DEEP_DIVE.md) | WebRTC 深入技术剖析 |
| [AI_VISION_MODELS.md](ui/qt_ui_arm64/AI_VISION_MODELS.md) | AI 视觉模型训练与部署 |
| [MCU_SOFTWARE_DESIGN.md](ui/qt_ui_arm64/MCU_SOFTWARE_DESIGN.md) | MCU 软件系统设计 |
| [MCU_SERIAL_PROTOCOL.md](ui/qt_ui_arm64/MCU_SERIAL_PROTOCOL.md) | MCU 串口通信协议 |
| [DEMO_SCRIPT.md](ui/qt_ui_arm64/DEMO_SCRIPT.md) | 演示视频拍摄脚本 |
| [EDITING_GUIDE.md](ui/qt_ui_arm64/EDITING_GUIDE.md) | 视频剪辑分镜指南 |
| [DEPLOY.md](ui/qt_ui_arm64/DEPLOY.md) | 机器人端部署步骤 |

---

## 构建

### 机器人端 (ARM64)

```bash
cd apps/ui/qt_ui_arm64
bash build_arm64.sh
# 输出: build-arm64/ui_rk (aarch64 ELF)
```

### PC 端 (x64)

```bash
cd apps/ui/qt_ui_x64
bash build_x64.sh
# 输出: build-x64/ui_x64 (x86-64 ELF)
```

### 信令服务器

```bash
cd apps/peerconnection/server
make peerconnection_server
# ./peerconnection_server --port=8888
```

---

## License

本项目基于 WebRTC BSD License 开源。AI 模型权重文件未包含在此仓库中。

---

