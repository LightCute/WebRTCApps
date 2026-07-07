# WebRTC 音视频通话 — 深入技术剖析

## 一、媒体管线：从采集到网络发送

### 1.1 整体数据流

```
┌──────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────┐
│ 摄像头    │    │  RGA 硬件     │    │  MPP 硬件     │    │  RTP 打包 │
│ V4L2     │───►│  YUYV→NV12   │───►│  H264 编码    │───►│  FU-A 分片│
│ 640×480  │    │  DMA-BUF     │    │              │    │          │
│ YUYV     │    │  零 CPU 拷贝  │    │              │    │          │
└──────────┘    └──────────────┘    └──────────────┘    └─────┬────┘
                                                              │
                                              ┌───────────────┘
                                              ▼
┌──────────┐    ┌──────────────┐    ┌──────────────────────────────┐
│ 网络发送  │    │  Pacer 平滑  │    │  RTPSenderVideo              │
│ UDP      │◄───│  按码率间隔   │◄───│  加 RTP 头 + FEC 保护        │
│ Socket   │    │  发送         │    │  存入 RtpPacketHistory        │
└──────────┘    └──────────────┘    └──────────────────────────────┘
```

### 1.2 采集：V4L2 → RGA 硬件转换

摄像头通过 Linux V4L2 框架采集，输出 YUYV 格式原始帧。系统通过 `VideoCaptureModuleV4L2` 封装 V4L2 的 `ioctl` 操作（`VIDIOC_DQBUF`/`VIDIOC_QBUF`），以 `V4L2_MEMORY_MMAP` 方式将内核 DMA 缓冲区直接映射到用户态，避免内核态到用户态的数据拷贝。

紧接着，RGA (Raster Graphic Acceleration) 硬件模块执行像素格式转换——YUYV → NV12。不同于 CPU 上的 `libyuv` 转换，RGA 通过 DMA 引擎直接将源缓冲区的像素搬运到目标 DMA-BUF 缓冲区中。转换后的 NV12 数据存放在 CMA (Contiguous Memory Allocator) 分配的物理连续内存中，RK3588 的各个硬件模块（RGA、MPP 编码器、GPU）均可通过 DMA-BUF 文件描述符 (fd) 直接访问，无需 CPU 参与数据搬运。

**代码证据**：[rga_video_track_source.cc:186-208](apps/client_arm64_linux/rga_video_track_source.cc#L186-L208) — 源 `src.virAddr` 为 V4L2 mmap 指针，目标 `dst.fd` 为 DMA-BUF fd。RGA 完成 blit 后，创建 `Nv12DmaBufBuffer` 包装该 fd。

### 1.3 零拷贝帧传递：Nv12DmaBufBuffer

WebRTC 标准管线中，视频帧通过 `VideoFrame` 对象在模块间传递，其内部持有 `VideoFrameBuffer` 的引用计数智能指针。本项目自定义了 `Nv12DmaBufBuffer` 类：

```cpp
class Nv12DmaBufBuffer : public webrtc::NV12Buffer {
    int fd_;  // DMA-BUF 文件描述符
    // 继承自 NV12Buffer，在 WebRTC 管线中表现为普通 NV12 帧
    // 但通过 fd_ 携带了 GPU/硬件可直接访问的零拷贝引用
};
```

**代码证据**：[rga_video_track_source.cc:27-45](apps/client_arm64_linux/rga_video_track_source.cc#L27-L45)

当编码器收到该帧时，通过 `GetNv12DmaBufFd()` 提取 fd，直接传递给 MPP 编码器硬件模块，实现从采集到编码的零 CPU 拷贝链路。

### 1.4 编码：MPP 硬件 H264

RK3588 的 MPP (Media Process Platform) 提供硬件 H264 编码。通过 `dlopen("librockchip_mpp.so.1")` 动态加载 MPP 库，避免编译时依赖。

编码流程：
1. 创建 MPP 编码上下文，配置 H264 Baseline Profile
2. 配置码率控制 (CBR, Constant Bitrate)
3. 传入 NV12 帧（通过 DMA-BUF fd 或 CPU buffer）
4. MPP 异步编码，回调返回 H264 码流

**代码证据**：[rk_mpp_encoder.cc:145-178](apps/client_arm64_linux/rk_mpp_encoder.cc#L145-L178) — `InitEncoder()` 配置编码参数，`EncodeOne()` 执行单帧编码。

### 1.5 RTP 打包与分片

H264 编码器输出的是 NAL 单元 (Network Abstraction Layer Units)。单个 NAL 单元可能超过网络 MTU（典型值约 1200 字节），需要通过 FU-A (Fragmentation Unit A) 方式分片。

**分片原理**：

```
原始 H264 NAL Unit (例如 4000 bytes):
┌──────────────────────────────────┐
│ NAL Header │     NAL 载荷        │
└──────────────────────────────────┘

FU-A 分片为多个 RTP 包:
┌─────────┬────────────────────────┐
│FU ind.  │ FU Header (S=1, E=0)  │ ← 第一个 RTP 包 (Start bit)
└─────────┴────────────────────────┘
┌─────────┬────────────────────────┐
│FU ind.  │ FU Header (S=0, E=0)  │ ← 中间 RTP 包
└─────────┴────────────────────────┘
┌─────────┬────────────────────────┐
│FU ind.  │ FU Header (S=0, E=1)  │ ← 最后一个 RTP 包 (End bit, Marker=1)
└─────────┴────────────────────────┘
```

**代码证据**：[rtp_format_h264.h:37-95](modules/rtp_rtcp/source/rtp_format_h264.h#L37-L95) — `RtpPacketizerH264` 实现 FU-A 分片逻辑。

每个 RTP 包包含：

| 字段 | 位数 | 说明 |
|------|------|------|
| V (版本) | 2 | 固定为 2 |
| P (填充) | 1 | 是否尾部填充 |
| X (扩展) | 1 | 是否有扩展头 |
| CC (CSRC 数量) | 4 | |
| M (Marker) | 1 | 帧的最后一个包置 1 |
| PT (载荷类型) | 7 | H264 通常用 96 |
| 序列号 | 16 | 每包递增，用于丢包检测 |
| 时间戳 | 32 | 同一帧的所有包共享相同时间戳 (90kHz) |
| SSRC | 32 | 同步源标识 |
| RTP 扩展头 | 变长 | Transport Sequence Number 等 |
| 载荷 | 变长 | H264 FU-A 数据 |

**代码证据**：[rtp_packet.h:222-234](modules/rtp_rtcp/source/rtp_packet.h#L222-L234) — RTP 包头字段定义。

### 1.6 Pacer：平滑发送

编码完成后，`RTPSenderVideo::SendVideo()` 将编码帧切分为 RTP 包序列，通过 `PacingController`（Pacer）按目标码率均匀间隔发送。例如 2Mbps 码率下，每个 ~1200 字节的包间隔约 4.8ms 发送。这避免了突发流量导致的网络丢包和拥塞。

**代码证据**：[rtp_sender.cc:489-502](modules/rtp_rtcp/source/rtp_sender.cc#L489-L502) — `EnqueuePackets()` 入队到 `paced_sender_`。

### 1.7 网络发送

Pacer 按时间间隔弹出 RTP 包 → `PacketRouter` 路由到正确的 SSRC → `RtpRtcp` 模块 → `Transport::SendRtp()` → UDP Socket `sendto()`。

**代码证据**：[packet_router.cc:171](modules/pacing/packet_router.cc#L171) — `SendPacket()` 执行最终路由和发送。

---

## 二、丢包重传 (NACK + RTX)

### 2.1 机制概述

在实时音视频通信中，TCP 的重传机制延迟太高，UDP 本身不保证可靠送达。WebRTC 使用 NACK (Negative ACKnowledgment) 机制处理丢包：

```
发送端                              接收端
   │  RTP seq=1,2,3,4,5 ──────────► │
   │                    (seq=3 丢失)  │
   │                                  │ 检测到序列号间隙 (收到4但缺3)
   │  ◄──── RTCP NACK: seq=3 ─────── │ 等待1个RTT后发送NACK
   │                                  │
   │  从RtpPacketHistory取出seq=3    │
   │  包装为RTX重传包 ─────────────► │
   │                                  │ 收到重传包，解码
```

### 2.2 发送端：包历史存储

每发送一个 RTP 包，同时存入 `RtpPacketHistory`。这是一个环形缓冲区，最多存储 9600 个包。每个包记录发送时间、重传次数和是否在 Pacer 队列中。

包的保留时间为 `max(1秒, 3×RTT)`。超过此时间的包被剔除以释放内存——因为接收端在超时后不会再请求该包。

**代码证据**：[rtp_packet_history.h:34-196](modules/rtp_rtcp/source/rtp_packet_history.h#L34-L196)

### 2.3 发送端：接收 NACK 并重传

收到接收端发来的 RTCP NACK 反馈包（包含丢失包的序列号列表），触发 `ReSendPacket()`：

1. 从 `RtpPacketHistory` 中查找对应序列号的包
2. 检查重传速率限制（避免带宽被重传占满）
3. 如果启用了 RTX，用 `BuildRtxPacket()` 包装——使用独立的 SSRC 和 PayloadType，在前面附加 2 字节 OSN (Original Sequence Number)
4. 将重传包重新入队 Pacer

**代码证据**：[rtp_sender.cc:278-324](modules/rtp_rtcp/source/rtp_sender.cc#L278-L324) — `ReSendPacket()` 完整流程。

### 2.4 RTX (Retransmission) 封装

RTX 是 WebRTC 标准的重传格式——重传包使用独立的 SSRC（与媒体流不同）和独立的 PayloadType，在 RTP 头后面附加原始序列号 (OSN)，以便接收端能正确还原原始序列号并填入抖动缓冲区。

### 2.5 接收端：NACK 生成

接收端的 `NackModule` 持续追踪收到的 RTP 序列号。当检测到间隙（如收到 seq=4 但 seq=3 未到），启动计时器。等待约一个 RTT 后（避免因网络乱序而误报），如果缺失包仍未到达，生成 RTCP NACK 消息发送给对端。

---

## 三、SDP / ICE 交换与公网 NAT 穿透

### 3.1 信令流程

WebRTC 本身只定义了媒体传输协议，不规定信令方式。本项目使用自定义的基于 Unix Socket 的控制协议 + 公网信令服务器。

```
    PC 端 (x64)                  信令服务器                  机器人端 (ARM64)
        │                     120.79.210.6:8888                  │
        │                            │                           │
        │── connect ────────────────►│◄──── connect ──────────── │
        │                            │                           │
        │── createOffer() ──────┐    │                           │
        │   (生成本地 SDP)       │    │                           │
        │   setLocalDescription()│   │                           │
        │                        ▼    │                           │
        │── SDP Offer ───────────────►│── SDP Offer ────────────►│
        │                            │   setRemoteDescription()  │
        │                            │   createAnswer()          │
        │                            │                           │
        │◄─ SDP Answer ──────────────│◄── SDP Answer ─────────── │
        │   setRemoteDescription()   │                           │
        │                            │                           │
        │── ICE Candidates ──────────│── ICE Candidates ────────►│
        │◄─ ICE Candidates ──────────│◄── ICE Candidates ─────── │
        │                            │                           │
        │◄══════ WebRTC P2P Connection Established ═══════════►│
```

### 3.2 SDP (Session Description Protocol)

SDP 描述了媒体会话的元信息：
- 媒体类型（video/audio）
- 编码格式（H264、Opus 等）
- 媒体流的 IP 和端口
- ICE 候选地址

双方交换 SDP Offer/Answer 后，各自知道对方支持的编解码器、媒体格式和候选网络地址。

### 3.3 ICE (Interactive Connectivity Establishment)

ICE 的目标是在复杂的 NAT 网络环境中找到可用的通信路径。它收集三种候选地址：

| 类型 | 说明 | 示例 |
|------|------|------|
| Host | 本机网卡 IP | 192.168.1.100 |
| SRFLX (Server Reflexive) | 通过 STUN 服务器获取的公网映射地址 | 203.0.113.5:45678 |
| Relay | 通过 TURN 服务器中继 | turn.example.com:3478 |

ICE 通过**连通性检查** (STUN Binding Request/Response) 测试所有候选地址对，选择延迟最低且连通的一对建立 P2P 连接。

### 3.4 NAT 穿透策略

优先级从高到低：

1. **直连**：双方在同一局域网或一方有公网 IP → 直接 UDP 通信
2. **STUN 穿透**：通过 STUN 服务器获取公网映射地址，利用 UDP Hole Punching 打洞（适用于 Cone NAT）
3. **TURN 中继**：当双方都在对称 NAT 后无法打洞时，通过 TURN 服务器中继所有媒体流（延迟和带宽开销最大，但确保连通性）

本项目的信令服务器部署在公网 `120.79.210.6:8888`，同时配置 TURN 服务器作为最后的连通保障。

---

## 四、WebRTC 协议栈总结

```
┌──────────────────────────────────────────────┐
│                应用层                         │
│  音视频通话 / DataChannel 控制 / AI 告警推送  │
├──────────────────────────────────────────────┤
│              WebRTC API 层                    │
│  PeerConnection / MediaStream / DataChannel  │
├──────────────────────────────────────────────┤
│              会话管理层                        │
│  SDP 协商 / ICE 连接建立 / DTLS 加密          │
├──────────────────────────────────────────────┤
│              传输层                           │
│  RTP/RTCP (媒体)  /  SCTP (DataChannel)      │
│  NACK 丢包重传 /  FEC 前向纠错               │
│  Pacer 平滑发送 /  拥塞控制 (GCC)             │
├──────────────────────────────────────────────┤
│              网络层                           │
│  UDP / STUN / TURN / ICE                     │
│  NAT 穿透 / P2P 连接                         │
├──────────────────────────────────────────────┤
│              物理层                           │
│  WiFi / 以太网 / 4G                          │
└──────────────────────────────────────────────┘
```
