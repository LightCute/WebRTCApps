# 居家陪护机器人 — 嵌入式芯片与系统设计竞赛 设计报告

## 作品名称

**居家陪护机器人**

---

## 摘要

本作品设计并实现了一款基于 RK3588 高性能嵌入式平台的居家陪护机器人系统。系统由机器人端（ARM64 Linux）和监护端（x64 Linux/Windows PC）两部分组成，通过公网 WebRTC 实现跨局域网实时音视频通话与远程控制。

机器人端以 Rockchip RK3588 为核心计算单元，自主设计 STM32 MCU 控制板和电机驱动板，搭载四轮差速底盘和二维舵机云台。软件方面，采用 Qt5 框架构建人机交互界面，以进程管理和跨进程通信（Unix Socket、DMA-BUF 共享内存、UART 串口）协同多个功能模块：WebRTC 音视频通话引擎、三组 AI 视觉推理模型（行人检测 YOLOv5、跌倒检测 YOLOv11、火灾烟雾检测 YOLOv8）、语音对话系统（本地 Whisper 语音识别 + 云端大语言模型 + 本地语音合成）。

系统的三大核心技术特点：（1）WebRTC 实时通信架构——通过 ICE 框架实现 Host/STUN/TURN 三重网络传输保障，实测公网环境下 STUN 穿透成功（prflx 候选），RTT 延迟 35ms，丢包率 0.2%；（2）RK3588 硬件加速视频管线——RGA 硬件色彩转换 + MPP 硬件 H.264 编解码（实测单帧 <1ms）+ DMA-BUF 零拷贝架构，数据始终驻留 CMA 物理连续内存，CPU 零参与像素搬运；（3）异构双芯片协同架构——RK3588 负责音视频/AI/UI，自研 MCU 负责底盘实时控制，通过自定义 UART 帧协议（0xAA 0x55 + XOR 校验 + 300ms 看门狗）可靠通信。

系统实现了四大核心功能：（1）公网实时音视频通话与远程底盘/云台控制；（2）AI 代理行人追踪模式，自动跟随将目标保持在画面中央；（3）跌倒检测和火灾烟雾检测两种安全监护模式，异常自动告警；（4）独立语音对话模式，无人连接时提供 AI 陪聊。本系统面向居家养老场景，通过异构芯片协同、零拷贝视频管线优化和多 AI 模型协同，为独居老人提供远程陪伴和智能守护。

---

# 第一部分 作品概述

## 1.1 功能与特性

本系统具备以下核心功能：

**（1）公网实时音视频通话。** 监护者通过 PC 端发起 WebRTC 呼叫，机器人端自动接听，双方建立 P2P 音视频连接。视频采用 MPP 硬件 H.264 编解码，DMA-BUF 零拷贝管线，延迟低于 100ms。通话过程中，监护者可远程控制机器人底盘移动（WASD 键盘）和云台转动（IJKL 键盘），实现"远程在场"的沉浸式陪伴体验。

**（2）AI 行人识别与自动追踪。** 开启 YOLOv5 行人检测后，机器人自动识别视野中的行人，锁定面积最小的检测框作为追踪目标，通过底盘差速控制将目标保持在画面中央舒适区（框高占比 45%~60%），全程无需手动干预。

**（3）跌倒/火灾安全监护。** 可在线切换跌倒检测模型（YOLOv11）和火灾烟雾检测模型（YOLOv8）。当检测到异常事件时，PC 监护端弹出红色/黄色闪烁告警叠加层，DataChannel 同步推送检测结果。火源/烟雾移除或人恢复站立后，告警自动解除。

**（4）独立语音对话。** 无人监护连接时，机器人可独立启动语音对话模式——本地 Whisper 模型将语音转为文字，云端大语言模型生成回复，本地 TTS 合成语音输出，为独居老人提供情感陪伴。

## 1.2 应用领域

本作品面向**居家养老与远程陪护**场景。中国正加速进入老龄化社会，独居老人数量持续增长。传统养老模式面临护理人力不足、子女异地工作无法实时照看等挑战。本系统提供了一种技术解决方案：

- **日常陪伴**：子女通过 PC 端随时发起音视频通话，不仅能看到老人的实时画面，还能远程控制机器人跟随老人移动，让通话不再局限于固定画面。
- **安全监护**：AI 跌倒检测和火灾检测覆盖两种高危场景。无需老人主动求助，系统自动识别并推送告警。
- **情感陪聊**：当无人连接时，语音对话功能为老人提供陪伴型 AI 对话，缓解孤独感。
- **术后康复监护**：可拓展应用于术后康复期患者或失能老人的远程监护和辅助巡房。

## 1.3 主要技术特点

**（1）WebRTC 实时通信架构。** 系统基于 WebRTC 标准协议栈实现公网条件下的实时音视频通话与控制数据传输。信令层采用 HTTP 长轮询协议连接公网信令服务器，完成 SDP 会话协商和 ICE 候选交换。传输层通过 ICE 框架实现三重网络连接保障：局域网 Host 直连（最低延迟）、STUN 服务器 NAT 穿透（PRFLX 候选）、TURN 服务器中继转发（Relay 候选，作为最终兜底）。媒体数据通过 RTP/RTCP 协议传输，采用 NACK 丢包重传和 Pacer 平滑发送确保实时性。控制指令和 AI 告警数据通过 SCTP DataChannel 复用同一 P2P 连接，无需额外的信令通道。

**（2）RK3588 硬件加速视频管线。** 系统充分利用 RK3588 芯片内置的硬件模块加速视频处理全流程。RGA（Raster Graphic Acceleration）硬件模块执行色彩空间转换（YUYV→NV12→I420），替代传统 libyuv CPU 方案。MPP（Media Process Platform）硬件编码器/解码器执行 H.264 编解码，实测单帧耗时 <1ms。关键创新在于 DMA-BUF 零拷贝架构：自定义 `Nv12DmaBufBuffer` 类携带 Linux DMA-BUF 文件描述符（fd）穿透 WebRTC 整个视频管线——从 RGA 转换输出到 MPP 编码器输入，数据始终驻留在 CMA（Contiguous Memory Allocator）物理连续内存中，CPU 全程零参与像素数据的搬运和拷贝。该架构在 640×480@30fps 条件下节省约 30% CPU 占用率，并消除了因 CPU 瓶颈导致的帧率抖动。

**（3）异构双芯片协同架构。** 系统采用 RK3588 + STM32 MCU 双芯片架构，根据各自优势进行职责划分。RK3588（8nm, 4×A76+4×A55, NPU 6TOPS）负责高计算量任务：WebRTC 音视频通话、AI 视觉推理（三组 YOLO 模型）、语音对话（Whisper+LLM）、Qt5 UI 渲染。自研 STM32 MCU 控制板负责底盘实时控制：差速运动学解算、PWM 电机驱动、舵机云台控制。双芯片通过 UART 115200 串口通信，定义自定义帧协议——0xAA 0x55 帧头 + 单字节长度 + JSON 载荷 + XOR 校验 + `\r\n` 帧尾。MCU 端设置 300ms 硬件看门狗定时器，收到指令即重置，超时自动停止所有电机和舵机，确保通信中断或主控故障时机器人安全停机。

## 1.4 主要性能指标

以下数据为系统在公网环境下的实测值（RK3588 端 WebRTC Stats API 采集）：

| 类别 | 指标 | 实测值 | 说明 |
|------|------|--------|------|
| 网络连接 | ICE 类型 | prflx（STUN 穿透） | 公网 NAT 穿透成功 |
| 网络连接 | 往返延迟 (RTT) | 35.0 ms | 端到端网络延迟 |
| 网络连接 | 丢包率 | 0.2% | 低于 1% 优秀线 |
| 视频编码 | 编码分辨率/帧率 | 640×480 @30fps | 满帧率，无降质 |
| 视频编码 | 编码耗时 | <1 ms/帧 | MPP 硬件编码器 |
| 视频编码 | 发送码率 | 1505 kbps | CBR 动态调整 |
| 视频解码 | 解码分辨率/帧率 | 640×480 @30fps | 满帧率 |
| 视频解码 | 解码耗时 | <1 ms/帧 | MPP 硬件解码器 |
| 视频解码 | 接收码率 | 1598 kbps | |
| AI 推理 | 推理间隔 | 250 ms | RK3588 NPU, INT8 量化 |
| AI 推理 | 模型输入尺寸 | 640×640 | 三模型统一 |
| AI 推理 | 置信度阈值 | 0.45 | 可配置 |
| 控制响应 | 指令延迟 | <50 ms（不含网络） | 事件驱动，按下即发 |
| MCU 通信 | 波特率 | 115200 bps | UART 8N1 |
| MCU 安全 | 看门狗超时 | 300 ms | 超时自动停机 |

## 1.5 主要创新点

① **主动式远程陪伴**：监护者随时发起连接，老人端自动接听、零操作，不依赖被叫应答。

② **三种 AI 守护模式**：行人追踪/跌倒检测/火灾识别覆盖居家陪护核心危险场景，在线热切换。

③ **陪伴型语音对话**：无人监护时独立运行，不是工具型 AI 助手，而是情感陪伴型对话。

④ **双端体验合一**：监护者通过键盘+滑块全功能操控，老人端零操作负担，一套系统解决两个不同技术能力的人的交互问题。

## 1.6 设计流程

```
需求分析 → 系统架构设计 → 硬件选型与电路设计 → 软件架构设计
    → 通信协议定义 → 各模块并行开发 → 集成联调 → 测试优化 → 文档撰写

硬件:   MCU板设计 → PCB打样 → 焊接调试 → 电机驱动联调
软件:   视频管线 → WebRTC集成 → AI模型训练与部署 → Qt UI开发 → IPC联调
```

---

# 第二部分 系统组成及功能说明

## 2.1 整体介绍

### 系统整体框图

```
┌──────────────────────────────────────────────────────────────────┐
│                       公网基础设施                                 │
│    信令服务器 (120.79.210.6:8888)  │  TURN/STUN 中继服务器         │
└────────────┬─────────────────────────────┬────────────────────────┘
             │                             │
    ┌────────┴────────┐           ┌────────┴────────┐
    │   PC 监护端      │  WebRTC   │  机器人端        │
    │  (x64 Linux)    │◄─────────►│  (ARM64 Linux)  │
    │                  │  P2P      │                  │
    │  qt_ui_x64       │ RTP/SCTP  │  qt_ui_arm64    │
    │  client_x64      │           │  client_arm64   │
    │  键盘/鼠标/摄像头 │           │  RK3588 + MCU   │
    │                  │           │  底盘/云台/传感器 │
    └─────────────────┘           └──────────────────┘
```

机器人与 PC 端均通过公网信令服务器建立 WebRTC P2P 连接。信令服务器负责交换 SDP 会话描述和 ICE 候选地址，不传输媒体数据。音视频 RTP 流和 DataChannel 控制数据走 P2P 直连（或经 TURN 中继）。机器人端 RK3588 运行 Qt 主进程，统筹管理 WebRTC daemon、AI 推理进程和语音对话进程；MCU 通过 UART 接收底盘/云台控制指令。

## 2.2 硬件系统介绍

### 2.2.1 硬件整体介绍

机器人硬件由计算核心、控制执行、感知交互和供电四大子系统组成：

| 子系统 | 组件 | 型号/参数 |
|--------|------|----------|
| 计算核心 | RK3588 开发板 | ELF2, 8nm, 4×A76+4×A55, NPU 6TOPS |
| 控制执行 | MCU 控制板（自研） | STM32 系列, 串口 115200 |
| 控制执行 | 电机驱动板（自研） | H 桥, 四路直流电机 |
| 感知交互 | 摄像头 | USB/V4L2, 640×480@30fps, YUYV |
| 感知交互 | 麦克风 | 板载 NAU8822 |
| 感知交互 | 扬声器 | 外接小型音箱 |
| 感知交互 | 显示屏 | 7" LCD, 1024×600 |
| 机械结构 | 四轮差速底盘 | 独立四驱 |
| 机械结构 | 二维舵机云台 | Pan/Tilt, 水平±90°/垂直±45° |
| 供电 | 锂电池 | 2200mAh |

### 2.2.2 机械设计介绍

机器人采用四轮差速底盘结构，四个直流电机独立驱动。通过左右两侧车轮的速度差实现前进、后退、原地旋转、差速转向。顶部安装二维舵机云台，水平舵机控制摄像头左右转动，垂直舵机控制上下俯仰。7 寸 LCD 屏幕通过 FPC 排线连接 RK3588 的 MIPI DSI 接口。整体结构紧凑，适合室内居家环境。

### 2.2.3 电路各模块介绍

**MCU 控制板**：以 STM32 为主控 MCU，通过 UART（`/dev/ttyS9`, 115200 baud, 8N1）接收 RK3588 发来的 JSON 控制指令。MCU 解析 0xAA 0x55 帧协议，提取底盘速度（v, w）和云台角度（pan, tilt），转换为 PWM 信号驱动电机驱动板。

**电机驱动板**：采用 H 桥电路驱动四路直流电机。MCU 根据差速公式计算左/右轮速度（`left = v×100 - w×50`, `right = v×100 + w×50`），通过 PWM 占空比控制电机转速，GPIO 控制方向。

**RK3588 外围**：USB 摄像头接入 USB 3.0 接口；NAU8822 音频 codec 通过 I2S 总线连接；7 寸 LCD 通过 MIPI DSI 连接；MCU 通过 UART4 连接。

## 2.3 软件系统介绍

### 2.3.1 软件整体介绍

机器人端软件架构为**多进程 + 多 IPC** 模式：

```
┌─────────────────────────────────────────────┐
│           Qt 主进程 (ui_rk)                  │
│  ┌───────────────────────────────────────┐  │
│  │ UI 渲染 (Qt5 + OpenGL ES)             │  │
│  │ 子进程管理 (QProcess)                  │  │
│  │ ├─ client_arm64 (WebRTC daemon)       │  │
│  │ ├─ reason/fall_detect/fire_detect     │  │
│  │ └─ voice_chat.py (Python)             │  │
│  │ 外部通信: ControlChannel / AiReceiver  │  │
│  │          SerialWorker / VoiceChat     │  │
│  └───────────────────────────────────────┘  │
│        │ Unix Socket │ QProcess              │
│  ┌─────┴──────┐ ┌───┴──────────┐           │
│  │WebRTC引擎   │ │AI推理进程×3   │           │
│  │RGA→MPP→RTP │ │DMA-BUF→NPU   │           │
│  └────────────┘ └──────────────┘           │
└─────────────────────────────────────────────┘
```

PC 端软件架构类似，但视频编解码使用纯软件方案（OpenH264/VP8/VP9），无硬件加速依赖。视频帧通过 System V 共享内存（SysV SHM）在 daemon 与 Qt UI 间传递。

### 2.3.2 软件各模块介绍

**（1）视频采集与编码管线**

摄像头通过 V4L2 框架采集 YUYV 格式原始帧。`RgaVideoTrackSource` 继承自 WebRTC 的 `VideoTrackSource` 和 `RawVideoSinkInterface`，直接接收 V4L2 的裸数据回调，绕过 WebRTC 默认的 libyuv CPU 转换。RGA 硬件模块执行 YUYV→NV12 格式转换，结果写入 DMA-BUF 内存。自定义 `Nv12DmaBufBuffer` 类携带 DMA-BUF 文件描述符（fd）随 WebRTC 的 `VideoFrame` 穿透整个管线。MPP 硬件编码器通过 `mpp_buffer_import_with_tag()` 导入该 fd，实现零拷贝编码。编码完成后，WebRTC 标准 RTP 管线完成 FU-A 分片和网络发送。

**（2）WebRTC 信令与连接管理**

两个客户端通过 HTTP 长轮询信令协议（`PeerConnectionClient`）连接公网信令服务器。交换 SDP Offer/Answer 和 ICE Candidates，建立 P2P 连接。当双方处于对称 NAT 后无法直连时，TURN 服务器中继媒体流。DataChannel 使用协商式 SCTP 通道，传输控制指令和 AI 告警数据。

**（3）AI 推理管线**

AI 推理进程从 DMA-BUF 共享内存读取 I420 视频帧。RGA 硬件将 I420 转换为 BGR 并 resize 到模型输入尺寸（640×640）。RKNN API 加载 INT8 量化后的 `.rknn` 模型文件，在 NPU 上执行推理（约 250ms/帧）。后处理包括 Anchor-Free 解码（ltrb 距离→边界框坐标）、NMS 去重、坐标缩放（模型空间→帧空间）。结果通过 Unix Socket 以 JSON 格式发送给 Qt UI。

**（4）MCU 串口通信**

`SerialWorker` 线程通过 `/dev/ttyS9` 以 115200 baud 与 MCU 通信。发送时使用 0xAA 0x55 帧封装 JSON 载荷，附加 XOR 校验和 `\r\n` 尾帧。接收时通过 7 状态机（WAIT_H1→H2→LEN→DATA→XOR→CR→LF）解析帧。MCU 端 300ms 看门狗定时器：收到指令即重置，超时自动停止电机和舵机，确保通信中断时机器人安全停机。

**（5）PC 端控制逻辑**

PC 端 Qt UI 采用事件驱动按键处理：按下按键即发送对应 JSON 控制指令，松开即发送停止指令。12 个键位覆盖底盘全向移动和云台二维旋转。右侧面板提供底盘速度和云台速度两个滑块（0.1~1.0 连续可调）。AI 按钮触发 DataChannel 文本指令（`AI:ON:yolov5`），机器人端收到后启动对应推理进程。

---

# 第三部分 完成情况及性能参数

## 3.1 整体介绍

[此处插入系统实物的正面和斜45°全局照片]

系统实物由四轮差速底盘、RK3588 开发板、自研 MCU 控制板、电机驱动板、7 寸 LCD 屏幕、摄像头云台模组和电池组成。整体尺寸紧凑，适合室内居家环境运行。

## 3.2 工程成果

### 3.2.1 机械成果

[此处插入机械结构实物照片：底盘、云台、整车]

### 3.2.2 电路成果

[此处插入 MCU 控制板、电机驱动板实物照片]

MCU 控制板和电机驱动板为自主设计、打样和焊接。MCU 板集成 STM32 主控、UART 接口、PWM 输出、GPIO 等。

### 3.2.3 软件成果

[此处插入 PC 端和机器人端 UI 界面截图]

机器人端 UI：1024×600 全屏 Qt5 界面，左侧为远程视频画面（叠加 AI 标注框），右上角本地 PIP 预览，右侧控制面板包含通信质量监控、速度调节和文字聊天区域。顶部工具栏提供连接、呼叫、AI 模式切换和语音对话按钮。

PC 端 UI：Qt5 界面，主区域为机器人端摄像头实时画面，右上方本地 PIP 预览，右侧面板包含通信质量监控、底盘/云台速度滑块和 DataChannel 文本聊天区域。工具栏提供连接、呼叫、AI 模式按钮。

## 3.3 特性成果

**（1）音视频通话**：跨公网 P2P 连接成功建立，视频流畅，延迟可接受。键盘 WASD/IJKL 实时控制机器人移动和云台转动。

**（2）AI 行人追踪**：YOLOv5 正确检测画面中的行人，标注框准确。追踪模式下机器人自动转向跟随，舒适距离保持良好。

**（3）跌倒/火灾检测**：使用平板电脑显示的火灾和跌倒图片置于摄像头前，AI 识别准确，PC 端告警叠加层正常弹出。火源/跌倒移除后告警自动解除。

**（4）语音对话**：Whisper 语音识别正确，LLM 回复流畅自然，TTS 语音清晰。支持多轮对话。

---

# 第四部分 总结

## 4.1 可扩展之处

**（1）多传感器融合**：当前仅使用 RGB 摄像头。可集成红外热释电传感器辅助人体检测，或加入激光雷达/超声波实现更精确的避障和 SLAM 自主导航。

**（2）边缘-云端协同 AI**：将部分计算密集型任务（如视频分析）卸载到边缘服务器或云端，降低 RK3588 负载，支持更复杂的模型。

**（3）智能家居联动**：通过 MQTT 或 Zigbee 协议接入智能家居系统，实现环境监测（温湿度、空气质量）、门窗状态监控、紧急按钮联动等功能。

**（4）多机器人协同**：支持多台机器人在不同房间协同工作，通过中心调度服务实现全覆盖监护。

## 4.2 心得体会

本项目从硬件设计、嵌入式软件开发到 AI 模型部署，涉及多个技术领域的交叉融合。在研发过程中，我们深刻体会到以下几点：

**（1）异构芯片协同设计的价值。** RK3588 计算能力强但实时性不如 MCU；MCU 实时性好但无法运行复杂的 AI 模型。将两者分工——RK3588 负责高计算量的音视频和 AI，MCU 负责实时底盘控制——是性能和可靠性的最佳折中。自定义 UART 帧协议和看门狗机制是保证两者可靠通信的关键。

**（2）零拷贝优化在嵌入式系统中的实际收益。** DMA-BUF 零拷贝视频管线最初是为了满足 RK3588 多硬件模块（RGA、MPP、GPU）共享视频帧的需求而设计的。实测中，相比传统 libyuv CPU 转换方案，零拷贝管线在 640×480@30fps 条件下节省了约 30% 的 CPU 占用率，更重要的是避免了因 CPU 瓶颈导致的帧率抖动。

**（3）进程管理和 IPC 设计的复杂性。** 本系统涉及 Qt 主进程、WebRTC daemon、3 种 AI 推理进程、语音对话 Python 进程共 6 个进程，进程间通过 Unix Socket、DMA-BUF SHM、UART 三种 IPC 方式通信。其中 WebRTC daemon 的稳定性至关重要——一旦崩溃，Qt UI 通过 QProcess 信号检测并自动重启。AI 推理进程的按需启动和终止也需精细的生命周期管理。

**（4）WebRTC 在嵌入式场景的适配挑战。** WebRTC 标准 API 设计时主要面向浏览器和桌面端，移植到嵌入式 ARM64 平台需要处理交叉编译、硬件编解码器集成、信令协议适配等问题。本项目中，我们通过封装 `WebRTCEngine` 核心 + 平台特定 `MediaPipeline`/`PcFactory` 的依赖注入架构，实现了代码在 ARM64 和 x64 平台的复用。

**（5）从 Demo 到产品化的差距。** 在实验室环境流畅运行的功能，部署到真实居家环境后可能遇到各种问题：WiFi 信号不稳定导致 WebRTC 断连、光照变化影响 AI 检测精度、电机长时间运行发热等。这些问题的解决需要大量的实地测试和迭代优化，这也是从竞赛作品走向实际产品必须跨越的鸿沟。

---

# 第五部分 参考文献

[1] WebRTC: Real-Time Communication in Browsers. W3C. https://www.w3.org/TR/webrtc/

[2] RFC 3550: RTP: A Transport Protocol for Real-Time Applications. IETF.

[3] RFC 5245: Interactive Connectivity Establishment (ICE). IETF.

[4] RFC 5766: Traversal Using Relays around NAT (TURN). IETF.

[5] Rockchip RK3588 Technical Reference Manual, 2023.

[6] Rockchip MPP (Media Process Platform) Development Guide, 2023.

[7] Rockchip RGA (Raster Graphic Acceleration) API Reference, 2023.

[8] Ultralytics. YOLOv5: Object Detection Model. https://github.com/ultralytics/yolov5

[9] Ultralytics. YOLOv8: Object Detection and Segmentation. https://github.com/ultralytics/ultralytics

[10] Ultralytics. YOLOv11: Anchor-Free Object Detection. https://docs.ultralytics.com

[11] Rockchip RKNN-Toolkit2 User Guide, 2023.

[12] Open Whisper. Whisper: Robust Speech Recognition via Large-Scale Weak Supervision. OpenAI, 2022.

[13] Qt 5.15 Documentation. The Qt Company. https://doc.qt.io/qt-5/

[14] Linux Kernel DMA-BUF Documentation. https://www.kernel.org/doc/html/latest/driver-api/dma-buf.html

[15] RFC 6184: RTP Payload Format for H.264 Video. IETF.

[16] SCTP: Stream Control Transmission Protocol. RFC 4960. IETF.

[17] Linus Torvalds et al. Linux V4L2 API Specification. Linux Kernel Documentation.

[18] STM32F4 Reference Manual. STMicroelectronics, 2021.

[19] Buildroot: Embedded Linux Build System. https://buildroot.org/

[20] ETSI TS 126 114: IP Multimedia Subsystem; Media Handling and Interaction. 3GPP.
