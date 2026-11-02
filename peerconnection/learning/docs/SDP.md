# SDP 解读指南

SDP（Session Description Protocol）是 WebRTC 里最核心的**能力协商文档**：双方各自列出"我能用什么（媒体类型、编解码、传输、加密、ICE）"，answer 端再确认。

本文以一个纯 DataChannel 的 SDP 为例逐行解读，再说明加入音视频后的变化。

## 1. 逐行解读（纯 DataChannel 的 SDP）

示例 SDP：

```
v=0
o=- 1116501291823107041 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0
a=extmap-allow-mixed
a=msid-semantic: WMS
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
c=IN IP4 0.0.0.0
a=ice-ufrag:IWa0
a=ice-pwd:63Acka8ug20XsTI/cSHezrhz
a=ice-options:trickle
a=fingerprint:sha-256 3E:4A:FC:A5:58:34:41:C6:C6:59:12:AA:2F:01:05:8E:F2:E8:94:FE:3D:E5:7F:72:58:D5:8E:53:B1:D9:5E:7D
a=setup:actpass
a=mid:0
a=sctp-port:5000
a=max-message-size:262144
```

| 行 | 含义 | 重要性 |
|---|---|---|
| `v=0` | SDP 版本号（恒为 0） | 忽略 |
| `o=- <会话ID> <版本> IN IP4 127.0.0.1` | 会话发起者（origin） | 忽略 |
| `s=-` / `t=0 0` | 会话名 / 时间（WebRTC 固定占位） | 忽略 |
| `a=group:BUNDLE 0` | **BUNDLE**：多个 m-line 复用同一个传输通道，这里只有 `0` 一个 m-line | 关注 |
| `a=msid-semantic: WMS` | MediaStream 标识语义 | 一般 |
| `m=application 9 UDP/DTLS/SCTP webrtc-datachannel` | **m-line**：媒体类型 `application`（DataChannel）、端口 `9`（占位）、传输 `UDP/DTLS/SCTP` | **重点** |
| `c=IN IP4 0.0.0.0` | 连接地址占位（实际地址由 ICE candidate 决定） | 忽略 |
| `a=ice-ufrag:IWa0` | **ICE 用户名片段**（打洞鉴权） | **重点** |
| `a=ice-pwd:...` | **ICE 密码**（打洞鉴权） | **重点** |
| `a=ice-options:trickle` | 支持 trickle ICE（候选逐个发） | 关注 |
| `a=fingerprint:sha-256 ...` | **DTLS 证书指纹**（端到端加密身份校验） | **重点** |
| `a=setup:actpass` | DTLS 角色协商（actpass = 既可主动也可被动） | 关注 |
| `a=mid:0` | 该 m-line 的标识 | 关注 |
| `a=sctp-port:5000` | DataChannel 用的 SCTP 端口 | 一般 |
| `a=max-message-size:262144` | DataChannel 单条消息上限（256KB） | 一般 |

## 2. 开发者最需要关注什么

1. **m-line 的类型和数量**：一眼判断会话性质。
   - `m=application` → 纯数据通道
   - `m=audio` / `m=video` → 音视频
   - 多个 m-line = 多个媒体流

2. **ICE 凭据 `ice-ufrag` + `ice-pwd`**：ICE 打洞的"账号密码"，两端必须匹配才能建立 ICE 连接。对应日志里的 `Set ICE ufrag: xxx pwd: xxx`。

3. **DTLS 指纹 `fingerprint`**：端到端加密的"公钥指纹"，用于对端校验证书、防中间人攻击。对应日志里的 `Making certificate`。

4. **`a=setup`**：DTLS 角色。offer 通常 `actpass`（可协商），answer 通常是 `active` 或 `passive`。

5. **`a=ice-options:trickle`**：候选是否可逐个交换，还是全塞进 SDP。

6. **（音视频额外盯）编解码器协商**（见下节）。

## 3. 加入音视频后的变化

核心变化：**m-line 从 `application` 变成 `audio`/`video`，并多出一大堆编解码器和 RTP 相关行**。

典型音视频 SDP（简化 offer）：

```
v=0
o=- ... 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0 1                     ← 两个 m-line 复用传输

m=audio 9 UDP/TLS/RTP/SAVPF 111 63    ← 音频 m-line，111 等是编解码 payload 号
c=IN IP4 0.0.0.0
a=ice-ufrag:xxxx
a=ice-pwd:xxxx
a=fingerprint:sha-256 ...
a=setup:actpass
a=mid:0
a=sendrecv                            ← 方向：收发
a=rtcp-mux                            ← RTCP 复用
a=rtpmap:111 opus/48000/2             ← 编解码器：Opus 48kHz 双声道
a=fmtp:111 minptime=10;useinbandfec=1 ← Opus 参数
a=rtcp-fb:111 transport-cc            ← 拥塞控制反馈
a=extmap:1 urn:...ssrc-audio-level    ← RTP 头扩展
a=ssrc:123456 cname:...               ← 音频轨标识

m=video 9 UDP/TLS/RTP/SAVPF 96 97     ← 视频 m-line
c=IN IP4 0.0.0.0
a=ice-ufrag:xxxx
a=ice-pwd:xxxx
a=fingerprint:sha-256 ...
a=setup:actpass
a=mid:1
a=sendrecv
a=rtcp-mux
a=rtpmap:96 VP8/90000                 ← 编解码器：VP8
a=rtpmap:97 rtx/90000                 ← 重传（rtx）
a=fmtp:97 apt=96                      ← rtx 关联 VP8
a=rtcp-fb:96 goog-remb                ← 带宽估计反馈
a=ssrc:...
```

### 关键差异对比

| 差异点 | DataChannel | 音视频 |
|---|---|---|
| m-line 协议 | `UDP/DTLS/SCTP` | `UDP/TLS/RTP/SAVPF`（RTP over DTLS） |
| 媒体类型 | `application` | `audio` / `video` |
| 编解码器 | 无（SCTP 传任意字节） | `a=rtpmap` + `a=fmtp`（Opus/VP8/VP9/H264…） |
| 方向 | 无 | `a=sendrecv` / `sendonly` / `recvonly` |
| RTP 相关 | 无 | `a=ssrc`、`a=msid`、`a=rtcp-mux`、`a=rtcp-fb`、`a=extmap` |
| BUNDLE | `BUNDLE 0` | `BUNDLE 0 1`（多 m-line 打包） |
| 传输上限 | `a=max-message-size` | 无（RTP 分包） |

## 4. 一句话总结

SDP 本质是**能力协商文档**。调试建连时最要盯的四样：**m-line 类型、ice-ufrag/ice-pwd、fingerprint、a=setup**——它们决定了"这是不是我要的会话、加密能不能握手、ICE 能不能打通"。
