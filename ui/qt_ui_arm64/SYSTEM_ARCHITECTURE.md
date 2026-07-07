# 居家陪护机器人 — 系统架构文档

## 一、系统总体框架

```
┌─────────────────────────────────────────────────────────────────────┐
│                         公网基础设施                                  │
│                                                                     │
│   ┌──────────────┐          ┌──────────────────┐                    │
│   │  信令服务器    │          │  TURN/STUN 服务器  │                    │
│   │  (Signaling)  │          │  (Media Relay)    │                    │
│   │ 120.79.210.6  │          │  公网 IP          │                    │
│   │   :8888       │          │                  │                    │
│   └──────┬───────┘          └────────┬─────────┘                    │
│          │   SDP/ICE 交换             │  UDP 媒体流转发               │
│          │                           │  (NAT 穿透失败时备用)          │
└──────────┼───────────────────────────┼─────────────────────────────┘
           │                           │
    ┌──────┴──────┐             ┌──────┴──────┐
    │   PC 端     │             │  机器人端    │
    │  (监护者)    │◄──WebRTC──►│  (被陪护者)  │
    │             │  P2P 连接   │             │
    └─────────────┘             └─────────────┘

原理：双方客户端首先连接信令服务器，交换 SDP（会话描述协议）和 ICE 候选地址。
     信令服务器协助双方找到最优 P2P 路径（直连 > STUN > TURN 中继）。
     连接建立后，音视频 RTP 流直接 P2P 传输，不经过服务器。
     当双方处于对称 NAT 后无法直连时，TURN 服务器作为中继转发媒体流。
```

## 二、机械结构

```
                        ┌─────────────┐
                        │  7" LCD 屏幕 │  ← 显示监护端视频 + UI
                        │  (1024×600) │
                        └──────┬──────┘
                               │ FPC 排线
        ┌──────────────────────┼──────────────────────┐
        │              二维云台 (PTZ)                  │
        │         ┌───────────┴───────────┐           │
        │         │      摄像头 (V4L2)     │           │
        │         │      640×480 @30fps   │           │
        │         └───────────────────────┘           │
        │                      │                      │
        │              ┌───────┴───────┐              │
        │              │  RK3588 开发板 │              │
        │              │  (ELF2)       │              │
        │              │  + 外围电路    │              │
        │              └───────┬───────┘              │
        │         ┌────────────┼────────────┐         │
        │         │      UART (115200)      │         │
        │         │    /dev/ttyS9           │         │
        │    ┌────┴────┐              ┌─────┴────┐   │
        │    │ MCU 控制板│              │电机驱动板 │   │
        │    │ (自主设计)│◄──PWM/GPIO─►│          │   │
        │    └─────────┘              └─────┬────┘   │
        │                          ┌───────┼───────┐ │
        │                          │ 左前轮 │ 右前轮 │ │
        │                          │ 左后轮 │ 右后轮 │ │
        │                          └───────┴───────┘ │
        └─────────────────────────────────────────────┘

供电：2200mAh 锂电池，驱动全部系统
```

## 三、硬件清单

| 组件 | 型号/参数 | 职责 |
|------|----------|------|
| 核心板 | ELF2 RK3588 (8nm, 4×A76+4×A55, NPU 6TOPS) | 音视频处理、AI 推理、UI 渲染 |
| MCU | 自研控制板 (STM32 系列) | 底盘实时控制、传感器采集 |
| 电机驱动 | 自研驱动板 | H 桥驱动四路直流电机 |
| 摄像头 | USB/V4L2, 640×480@30fps, YUYV 输出 | 视频采集 |
| 麦克风 | 板载 NAU8822 | 音频采集（语音通话 + 语音对话） |
| 扬声器 | 外接小型音箱 | 音频输出 |
| 显示屏 | 7" LCD, 1024×600, 连接 RK3588 | 显示 UI 界面 + 远端视频 |
| 云台 | 二维舵机云台 (Pan/Tilt) | 挂载摄像头，水平/垂直旋转 |
| 底盘 | 四轮差速底盘 | 前进/后退/原地旋转 |
| 电池 | 2200mAh 锂电池 | 全系统供电 |
| 网络 | WiFi / 以太网 | 公网连接 |

## 四、软件架构：WebRTCEngine

WebRTCEngine 是 PC 端和机器人端 WebRTC 核心。**arm64 daemon 使用本地 fork 版本**（`apps/client_arm64_linux/webrtc_engine.h`），自包含构建；**x64 daemon 使用共享版本**（`apps/webrtc_engine/webrtc_engine.h`），通过依赖注入接收平台特定实现。两者接口一致。

**代码位置**：arm64 — [client_arm64_linux/main.cc:65](apps/client_arm64_linux/main.cc#L65)；x64 — [client_x64_linux/main.cc:49-54](apps/client_x64_linux/main.cc#L49-L54) 显式 SetMediaPipeline/SetPcFactory。

### 4.1 依赖注入架构

```
WebRTCEngine (共享)
    │
    ├─ SetMediaPipeline()  →  IMediaPipeline    (平台特定)
    │     ├─ arm64: MediaPipeline → RgaVideoTrackSource + MPP 硬编码
    │     └─ x64:   MediaPipeline → CapturerTrackSource + 软件编码
    │
    ├─ SetPcFactory()      →  IPcFactory         (平台特定)
    │     ├─ arm64: PcFactory → MPP 硬编码/硬解码
    │     └─ x64:   PcFactoryX64 → OpenH264/VP8/VP9/AV1 软件编解码
    │
    ├─ SetSignaling()      →  SignalingInterface  (共享)
    │     └─ PeerConnectionClient → WebSocket 连接信令服务器
    │
    └─ SetIpcServer()      →  IIpcServer         (平台特定)
          ├─ arm64: UnixSocketServer → /tmp/webrtc_runtime/webrtc_ctrl.sock
          └─ x64:   UnixSocketServer → /tmp/webrtc_runtime/dc_call.sock
```

### 4.2 Engine 内部组件

| 组件 | 类 | 职责 |
|------|-----|------|
| 信令客户端 | `PeerConnectionClient` | WebSocket 连接信令服务器，交换 SDP/ICE |
| P2P 连接 | `PeerConnection` | WebRTC 标准 P2P 连接 |
| 数据通道 | `DataChannelManager` | 管理一条协商式 DataChannel，收发 JSON 控制指令 |
| 媒体管线 | `IMediaPipeline` | 采集、编码、渲染（平台特定） |
| IPC 服务 | `IIpcServer` | Unix Socket 监听，接受 Qt UI 的控制命令 |
| 控制协议 | `ControlProtocol` | JSON-RPC 风格命令解析，将 UI 命令翻译为 Engine API 调用 |

### 4.3 Qt UI ↔ daemon 控制协议

Qt UI 通过 Unix Socket 连接到 daemon，发送 JSON 控制命令，格式为 `{"id":1,"method":"...","params":{...}}`。Engine 处理后通过 `ControlProtocol` 返回 `{"id":1,"ok":true}` 或 `{"id":1,"ok":false,"error":"..."}`。同时 Engine 将事件（如 ICE 状态变化、DataChannel 消息）推送给 UI：

```json
{"event":"ice_state","state":"connected"}
{"event":"data","text":"..."}
{"event":"peer_list","peers":[{"id":1,"name":"机器人"}]}
```

**代码证据**：[control_protocol.h](apps/webrtc_engine/control_protocol.h) — JSON-RPC 命令处理；[unix_socket_server.cc](apps/client_arm64_linux/unix_socket_server.cc) — Unix Socket IPC。

### 4.4 信令协议：PeerConnectionClient

信令客户端使用 **HTTP 长轮询** 协议，通过原始 TCP Socket 与信令服务器通信：

| 操作 | HTTP 请求 | 说明 |
|------|----------|------|
| 登录 | `GET /sign_in?<name> HTTP/1.0` | 返回在线 peer 列表（CSV 格式） |
| 等待通知 | `GET /wait?peer_id=<id> HTTP/1.0` | 长轮询，直到有新消息才返回 |
| 发送消息 | `POST /message?peer_id=<from>&to=<to> HTTP/1.0` | JSON body：SDP 或 ICE candidates |
| 挂断 | `GET /hangup?peer_id=<id> HTTP/1.0` | 通知对端挂断 |
| 登出 | `GET /sign_out?peer_id=<id> HTTP/1.0` | |

断连后 2 秒自动重连（`kReconnectDelay`）。默认服务器 `localhost:8888`，通过环境变量 `WEBRTC_SERVER` 覆盖。

**代码证据**：[peer_connection_client.cc](apps/webrtc_engine/peer_connection_client.cc)

### 4.5 Qt UI ↔ daemon 控制协议（JSON-RPC）

Qt UI 通过 Unix Socket 向 daemon 发送 JSON 命令：

```json
// 请求：{"cmd":"<方法>","id":<序列号>,"params":{...}}
{"cmd":"call","id":1,"params":{"peer_id":3}}
{"cmd":"send_data","id":2,"params":{"text":"w"}}

// 响应：{"id":1,"ok":true}
// 错误：{"id":1,"ok":false,"error":"Unknown command"}
```

daemon 主动推送的事件：

```json
{"event":"peer_list","peers":[{"id":1,"name":"机器人"}]}
{"event":"call_connected"}
{"event":"ice_state","state":"connected"}
{"event":"data_channel_state","state":"open"}
{"event":"data_received","text":"..."}
```

**代码证据**：[unix_socket_server.cc](apps/client_arm64_linux/unix_socket_server.cc) 处理 7 种命令：[connect/disconnect/call/hangup/send_data/shutdown/get_local_sdp](apps/client_arm64_linux/unix_socket_server.cc#L81-L135)。

### 4.6 DataChannelManager

PC 和机器人之间通过一条**协商式 SCTP DataChannel**（`id=0`, `ordered=true`）传输控制数据和 AI 告警。双方 daemon 启动时即创建，无需等待 call 建立——DataChannel 随 PeerConnection 的 SCTP 关联自动打开。

**代码证据**：[data_channel_manager.h](apps/webrtc_engine/data_channel_manager.h)；[webrtc_engine.cc:714-718](apps/webrtc_engine/webrtc_engine.cc#L714-L718) — `AddDataChannel()` 创建协商式通道。

### 4.7 平台编解码差异

| | arm64 (机器人) | x64 (PC) |
|---|---|---|
| 视频采集 | V4L2 + RGA 硬件 YUYV→NV12 | 软件 V4L2 → libyuv I420 |
| 视频编码 | **MPP 硬件** H264 | **OpenH264 软件** H264 |
| 视频解码 | **MPP 硬件** H264 | OpenH264 / VP8 / VP9 / AV1 软件 |
| 零拷贝 | DMA-BUF fd 穿透管线 | 无（标准 CPU buffer） |
| 视频渲染 | OpenGL ES + QPainter 标注框 | OpenGL + QPainter 标注框 |
| 本地预览 | DMA-BUF → SHM → Qt | SHM → Qt |

**代码证据**：[pc_factory.h](apps/client_arm64_linux/pc_factory.h) vs [pc_factory_x64.h](apps/client_x64_linux/pc_factory_x64.h)；[media_pipeline.cc](apps/client_arm64_linux/media_pipeline.cc) vs [media_pipeline.cc](apps/client_x64_linux/media_pipeline.cc)。

---

## 五、软件架构：机器人端 (RK3588)

### 5.1 进程拓扑

```
┌──────────────────────────────────────────────────────┐
│                    RK3588 (Linux)                     │
│                                                       │
│  ┌─────────────────────────────────────────────┐     │
│  │              Qt 主进程 (ui_rk)                │     │
│  │  ┌─────────────────────────────────────┐    │     │
│  │  │  UI 渲染 (Qt5 + OpenGL ES)          │    │     │
│  │  │  - 7" LCD 全屏显示 (1024×600)       │    │     │
│  │  │  - 远端视频 + 本地 PIP + AI 标注框   │    │     │
│  │  │  - 系统日志 + 对话记录               │    │     │
│  │  ├─────────────────────────────────────┤    │     │
│  │  │  子进程管理 (QProcess)               │    │     │
│  │  │  ├─ client_arm64 (WebRTC 引擎)      │    │     │
│  │  │  ├─ reason/fall_detect/fire_detect  │    │     │
│  │  │  │  (AI 推理, 按需启动)              │    │     │
│  │  │  └─ voice_chat.py (Python)          │    │     │
│  │  ├─────────────────────────────────────┤    │     │
│  │  │  外部通信                            │    │     │
│  │  │  ├─ ControlChannel → daemon 控制     │    │     │
│  │  │  ├─ AiReceiver ← AI 检测结果        │    │     │
│  │  │  ├─ VoiceChatClient ↔ 语音对话      │    │     │
│  │  │  ├─ SerialWorker → MCU 串口         │    │     │
│  │  │  └─ DmaBufVideoSource ← DMA-BUF 帧  │    │     │
│  │  └─────────────────────────────────────┘    │     │
│  └──────────────┬──────────┬───────────────────┘     │
│                 │          │                          │
│     Unix Socket │  QProcess│                          │
│                 │          │                          │
│  ┌──────────────┴──────────┴───────────────────┐    │
│  │  WebRTC daemon (client_arm64)                │    │
│  │  ┌─────────────────────────────────────┐    │    │
│  │  │  WebRTCEngine (共享核心)             │    │    │
│  │  │  ├─ MediaPipeline (arm64)           │    │    │
│  │  │  │   ├─ RgaVideoTrackSource         │    │    │
│  │  │  │   │   V4L2 + RGA + DMA-BUF       │    │    │
│  │  │  │   ├─ MppH264Encoder (MPP)        │    │    │
│  │  │  │   └─ MppH264Decoder (MPP)        │    │    │
│  │  │  ├─ PcFactory (arm64, MPP codecs)   │    │    │
│  │  │  ├─ PeerConnectionClient            │    │    │
│  │  │  ├─ DataChannelManager              │    │    │
│  │  │  └─ UnixSocketServer                │    │    │
│  │  └─────────────────────────────────────┘    │    │
│  │  监听: /tmp/webrtc_runtime/webrtc_ctrl.sock │    │
│  └──────────────────────────────────────────────┘    │
│                                                       │
│  ┌──────────────────────┐   ┌──────────────────────┐ │
│  │  AI 推理进程 ×3       │   │  语音对话进程 (Python) │ │
│  │  (RKNN / NPU)        │   │  Whisper + LLM + TTS  │ │
│  │  DMA-BUF→RGA→NPU     │   │                       │ │
│  │  → ai_detections.sock│   │                       │ │
│  └──────────────────────┘   └──────────────────────┘ │
└──────────────────────────────────────────────────────┘
```

### 5.2 进程间通信 (IPC) 方式一览

| 通信对象 | 方式 | 路径/参数 | 协议格式 |
|---------|------|----------|---------|
| Qt UI ↔ daemon (arm64) | Unix Socket | `/tmp/webrtc_runtime/webrtc_ctrl.sock` | JSON-RPC: `{"id":1,"method":"call","params":{...}}` |
| Qt UI ↔ daemon (x64) | Unix Socket + Pipe | `/tmp/webrtc_runtime/dc_call.sock` | JSON-RPC 控制协议 |
| Qt UI ↔ AI 推理 | Unix Socket | `/tmp/webrtc_runtime/ai_detections.sock` | JSON: `{"ts":...,"dets":[{"cls":0,"box":[...]}]}` |
| Qt UI ↔ MCU | UART | `/dev/ttyS9`, 115200 baud | 0xAA 0x55 帧封装 JSON |
| daemon → AI 推理 | DMA-BUF SHM | `/tmp/webrtc_runtime/shm_video_buf_local` | DMA-BUF fd (零拷贝) |
| daemon → 本地预览 | DMA-BUF SHM | `/tmp/webrtc_runtime/shm_video_buf_remote` | DMA-BUF fd + Seqlock |
| daemon ↔ daemon | WebRTC P2P | RTP (H264) + SCTP DataChannel | 标准 WebRTC 协议栈 |
| Qt UI ↔ 语音对话 | Unix Socket | `/tmp/webrtc_runtime/voice_chat.sock` | 文本消息，按行分隔 |

**注**：arm64 daemon 入口在 [client_arm64_linux/main.cc:65-78](apps/client_arm64_linux/main.cc#L65-L78)，x64 daemon 入口在 [client_x64_linux/main.cc:49-62](apps/client_x64_linux/main.cc#L49-L62)。两者共用 [webrtc_engine/webrtc_engine.h](apps/webrtc_engine/webrtc_engine.h) 中的 `WebRTCEngine` 核心。

### 4.3 视频管线（采集 → 编码 → 传输）

```
 摄像头 (V4L2)
   │ YUYV, 640×480@30fps
   ▼
 RgaVideoTrackSource
   │ RGA 硬件: YUYV → NV12 (DMA-BUF)
   │ 零拷贝: DMA-BUF fd 携带穿透 pipeline
   ├──► Nv12DmaBufBuffer → MPP H264 硬件编码 → RTP → 网络
   │
   └──► RGA 硬件: NV12 → I420 (DMA-BUF)
         │
         ├──► 本地预览 SHM (Qt UI 取帧渲染)
         │
         └──► DMA-BUF Socket → AI 推理进程 (RGA: I420→RGB→RKNN)
```

## 六、软件架构：PC 端 (x64)

```
┌──────────────────────────────────────────┐
│              PC (Linux/Windows)           │
│                                           │
│  ┌──────────────────────────────────┐    │
│  │        Qt 主进程 (ui_x64)         │    │
│  │  - UI 渲染 + 键盘事件处理         │    │
│  │  - 速度滑块 (底盘 + 云台)         │    │
│  │  - AI 告警叠加层渲染              │    │
│  │  - client_x64 管理 (QProcess)     │    │
│  │  - ControlChannel → daemon        │    │
│  │  - ShmVideoSource ← SHM 帧        │    │
│  └──────────────┬───────────────────┘    │
│                 │ Unix Socket             │
│  ┌──────────────┴───────────────────┐    │
│  │  WebRTC daemon (client_x64)       │    │
│  │  ┌───────────────────────────┐   │    │
│  │  │  WebRTCEngine (共享核心)    │   │    │
│  │  │  ├─ MediaPipeline (x64)   │   │    │
│  │  │  │   CapturerTrackSource   │   │    │
│  │  │  │   (软件 V4L2 + libyuv)  │   │    │
│  │  │  ├─ PcFactoryX64           │   │    │
│  │  │  │   (OpenH264/VP8/VP9)    │   │    │
│  │  │  ├─ PeerConnectionClient   │   │    │
│  │  │  ├─ DataChannelManager     │   │    │
│  │  │  └─ UnixSocketServer       │   │    │
│  │  └───────────────────────────┘   │    │
│  │  监听: /tmp/webrtc_runtime/       │    │
│  │        dc_call.sock               │    │
│  └──────────────────────────────────┘    │
└──────────────────────────────────────────┘
```

**PC 端的角色**：远程监护操作台。键盘 WASD/IJKL 控制机器人移动 + 云台，右侧面板调速 + AI 按钮，实时接收 AI 告警并弹出红色/黄色叠加层。PC 端使用软件编解码（OpenH264/VP8/VP9/AV1），不依赖特定硬件加速。

**代码证据**：[client_x64_linux/main.cc:31-78](apps/client_x64_linux/main.cc#L31-L78) — x64 daemon 启动流程，依赖注入 MediaPipeline + PcFactoryX64。

## 六、MCU 协议 (RK3588 ↔ MCU)

物理层：UART 115200, 8N1, `/dev/ttyS9`

帧格式：

```
┌──────┬──────┬──────┬───────────────────┬──────┬──────┬──────┐
│ 0xAA │ 0x55 │ LEN  │  JSON payload     │ XOR  │ 0x0D │ 0x0A │
│ 帧头1 │ 帧头2 │1 byte│    ≤255 bytes     │1 byte│  \r  │  \n  │
└──────┴──────┴──────┴───────────────────┴──────┴──────┴──────┘
```

命令类型：

| cmd | 用途 | JSON |
|-----|------|------|
| move | 底盘移动 | `{"cmd":"move","v":0.5,"w":0}` |
| ptz | 云台控制 | `{"cmd":"ptz","pan":0,"tilt":0.5}` |
| move_ptz | 底盘+云台同时 | `{"cmd":"move_ptz","v":0.5,"w":0.2,"pan":0,"tilt":0.5}` |
| stop | 停止 | `{"cmd":"stop"}` |
| ptz_home | 云台归中 | `{"cmd":"ptz_home"}` |

安全机制：MCU 端看门狗 300ms——收到指令重置定时器，超时自动停止所有电机/舵机。
PC 端每 80ms 保活发送，确保按住按键期间看门狗不触发。

## 七、数据流全景图

```
┌─────────────────────────────────────────────────────────────────────┐
│  数据流方向                    路径                       协议/格式   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  摄像头 → 编码器         V4L2→RGA→NV12 DMA-BUF→MPP    DMA-BUF fd  │
│                                                                     │
│  编码器 → 网络           MPP H264→RTP 打包            H264/RTP     │
│                                                                     │
│  网络 → 远端解码          RTP→MPP 解码→GL 渲染          H264/RTP     │
│                                                                     │
│  PC 按键 → 机器人底盘     PC UI→UnixSocket→daemon→DChan→       │
│                          →对端daemon→UnixSocket→arm64 UI        │
│                          →Serial→MCU→电机           JSON/UART    │
│                                                                     │
│  摄像头 → AI 推理         DMA-BUF SHM→RGA→RKNN NPU     DMA-BUF fd  │
│                                                                     │
│  AI 结果 → UI            Unix Socket→Qt→GL 标注框       JSON        │
│                                                                     │
│  AI 结果 → PC 告警       Qt→DChan→PC→告警叠加层         JSON        │
│                                                                     │
│  语音对话                 Mic→Whisper ASR→LLM API→TTS   PCM/HTTP    │
│                          →Speaker                                   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## 八、功能映射表

| 功能 | 连通状态 | 数据流路径 | 核心依赖 |
|------|---------|-----------|---------|
| 音视频通话 | PC↔机器人 已连接 | 摄像头→MPP→RTP→远端→渲染 | WebRTC, RGA, MPP |
| 远程底盘控制 | PC↔机器人 已连接 | PC键盘→DChan→daemon→UART→MCU→电机 | DataChannel, MCU协议 |
| 云台控制 | PC↔机器人 已连接 | PC键盘→DChan→daemon→UART→MCU→舵机 | DataChannel, MCU协议 |
| 行人追踪 | PC↔机器人 已连接 | 摄像头→AI推理→标注框+底盘跟随 | YOLOv5, RKNN NPU |
| 跌倒检测 | PC↔机器人 已连接 | 摄像头→AI推理→标注框+PC告警 | YOLOv11, RKNN NPU |
| 火灾检测 | PC↔机器人 已连接 | 摄像头→AI推理→标注框+PC告警 | YOLOv8, RKNN NPU |
| 语音对话 | 未通话，独立运行 | Mic→Whisper→LLM API→TTS→Speaker | Python, Whisper, LLM |

## 九、关键工程指标

| 指标 | 数值 | 说明 |
|------|------|------|
| 视频分辨率 | 640×480 @30fps | V4L2 摄像头原生输出 |
| 编码格式 | H.264 | MPP 硬件编码器 |
| 视频管线 | DMA-BUF 零拷贝 | RGA 硬件 blit, CPU 零参与 |
| AI 推理间隔 | 250ms | RK3588 NPU, 可配置 |
| AI 模型数量 | 3 种 | 行人检测 / 跌倒检测 / 火灾检测，在线热切换 |
| 控制延迟 | 按键事件驱动 | 按下即发，松开即停止（不含网络延迟） |
| MCU 协议 | 0xAA 0x55 帧, 115200 baud | XOR 校验，300ms 看门狗 |
| 语音对话 | Whisper 本地 + LLM 云端 | 多轮对话，上下文记忆 |
| 信令服务器 | 公网 IP, 8888 端口 | WebRTC NAT 穿透 |
| 跨平台 | Linux (ARM64 + x64) | Qt5 跨平台 UI |

---

## 十、附录：UI 信号/槽连接全景

### daemon 生命周期

```
WebRtcProcessManager::stateChanged → MainWindow::onProcessStateChanged
  Running: 500ms → ControlChannel::connectToServer()
  Stopped: disable hangup
  Error: disable hangup
WebRtcProcessManager::daemonLog → MainWindow::log
```

### 控制通道 → 信令

```
ControlChannel::serverConnected → 设 connected 状态，voice_chat_startup_complete_ = true
ControlChannel::serverDisconnected → 清 AI/语音/通话状态、标注框
ControlChannel::peerListReceived → 填充 peer_tree_ QTreeWidget
```

### 通话流程

```
ControlChannel::callConnected → is_call_active_=true, 启用 hangup, 禁用语音对话
ControlChannel::iceStateChanged → "connected" → switchToCallMode() + startVideoSources()
ControlChannel::dataChannelStateChanged → "open" → 启用 chat_input_/send_button_/AI 按钮
ControlChannel::callDisconnected → 停 AI/视频/语音, disconnect daemon, 1.5s 后重启
```

### DataChannel 消息

```
x64 端: dataReceived → "AI:ON_OK:yolov5" → 设 active type / "AI:DET:<json>" → 解析+渲染+checkDangerLabels
arm64 端: dataReceived → handleRemoteKey (运动JSON→串口) / "AI:ON:*" → launch AI 进程 / "AI:DET:*" → 渲染
```

### AI 检测 (arm64 only)

```
AiReceiver::detectionsReady → onAiDetections
  → local_video_->setDetections()
  → YOLOv5: findTrackTarget() + track_timer_->start()
  → 序列化 → channel_->cmdSendData("AI:DET:<json>")
track_timer_(150ms)::timeout → onTrackingTimer → sendSerialJson({"cmd":"move",...})
```

### 视频源

```
x64: ShmVideoSource::frameReady → local_video_/remote_video_->setFrame()
arm64: DmaBufVideoSource::frameReady → local_video_/remote_video_->setFrame()
```

### 语音对话

```
VoiceChatManager::start("python3 voice_chat.py --socket <path>")
VoiceChatClient::ready → cmdStart()
VoiceChatClient::asrResult → chatLog("You: " + text)
VoiceChatClient::llmResult → chatLog("Bot: " + text)
```

### 串口 (arm64 only)

```
SerialWorker::received → log("MCU: " + json)
SerialWorker::errorOccurred → log("Serial: " + msg)
```
