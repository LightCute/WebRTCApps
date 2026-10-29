# WebRTC 通话挂断与资源释放

> 本文汇总关于"通话挂断（hangup）"的设计讨论，回答四个核心问题：
> 1. 挂断用哪个接口？`StatsObserver` 是监测连接状态的接口吗？
> 2. 挂断是否必须 `Close()` 后销毁 PeerConnection？
> 3. `Close()` 之外，媒体管线（track / channel / 麦克风 / 摄像头）要不要停、要不要释放？
> 4. 重连是否要重建 PeerConnection？正确的挂断 / 重连顺序是什么？

---

## 一、先分清两个易混淆的接口

`peer_connection_interface.h` 里有两个名字很像"监测"的接口，职责完全不同：

| 接口 | 职责 | 关键回调 |
|------|------|---------|
| **`PeerConnectionObserver`** | **监测连接状态**（信令/ICE/连接） | `OnConnectionChange`、`OnSignalingChange`、`OnIceConnectionChange`、`OnIceGatheringChange` |
| **`StatsObserver`** | **拉取统计指标**（码率/丢包/RTT/抖动） | `OnComplete(const StatsReports&)` |

- `StatsObserver` 不是状态监测接口，它只服务于**旧版（legacy）`GetStats()`**，拿的是性能数据，且已标记 `deprecated`，新代码应用 `RTCStatsCollectorCallback` + 新版 `GetStats()`。
  - 见 [peer_connection_interface.h:172-178](api/peer_connection_interface.h#L172-L178)、[:925-961](api/peer_connection_interface.h#L925-L961)
- 监测连接状态用 `PeerConnectionObserver`，重点看 `OnConnectionChange(PeerConnectionState)`（`new/connecting/connected/disconnected/failed/closed`）。
  - 见 [peer_connection_interface.h:1250-1304](api/peer_connection_interface.h#L1250-L1304)

---

## 二、`Close()` 的语义：不可逆

`PeerConnectionInterface::Close()` 是**唯一**的挂断接口，注释明确写着 "This is an irreversible operation"。

- 见 [peer_connection_interface.h:1205-1211](api/peer_connection_interface.h#L1205-L1211)

`Close()` 之后会发生（由 WebRTC 单元测试 `CloseAndTestStreamsAndStates` 等确认）：

| 状态 | 结果 |
|------|------|
| Signaling state | → `kClosed` |
| ICE connection state | → `kIceConnectionClosed` |
| `PeerConnectionState` | → `kClosed` |
| 远端媒体 track | → `kEnded` |
| 缓存的 ICE candidate | 丢弃 |
| 后续方法调用 | 优雅失败（无效，但不再崩溃） |

**结论：`Close()` 是终态，closed 之后对象不能"复活"或复用，必须销毁重建。**

---

## 三、`Close()` 到底管什么、不管什么

这是整个讨论最关键的一点：**`Close()` 只管 RTP 传输层，不管物理采集层。**

```
┌───────────────────────────────────────────────┐
│ 传输层（RTP/SRTP 收发）── Close() 会停 ✅      │  sender/receiver/transceiver/channel
├───────────────────────────────────────────────┤
│ 采集层（麦克风/扬声器/摄像头）── Close() 不会停 ❌ │  ADM / capturer，归 factory/source 管
└───────────────────────────────────────────────┘
```

### `Close()` 内部会做的（源码 [peer_connection.cc:1929-1998](pc/peer_connection.cc#L1929-L1998)）

1. 状态置为 `kClosed` / `kIceConnectionClosed`。
2. 对每个未 stop 的 transceiver 执行 `GetStopTransceiverProcedure()` —— **停所有 sender/receiver**。
3. 关闭 SDP handler、media channel、transport。

### `Close()` 内部不会做的

1. **不碰 `AudioDeviceModule`（麦克风/扬声器）** —— `StopRecording()/StopPlayout()` 只在 `WebRtcVoiceEngine::Terminate()` 里出现（[webrtc_voice_engine.cc:558-575](media/engine/webrtc_voice_engine.cc#L558-L575)），而 `Terminate()` 只在 **PeerConnectionFactory 销毁**时触发。
2. **不碰本地 capturer（摄像头）** —— capturer 归 `VideoTrackSource` 持有（[conductor.cc:132-143](examples/peerconnection/client/conductor.cc#L132-L143)），只有 source 引用归零、析构时才停止。

---

## 四、挂断时三样东西"归谁停"

| 对象 | 挂断时谁负责停 | 需要手动"暂停/销毁"吗 |
|------|--------------|----------------------|
| **Audio/Video Track**（RTP 层） | `Close()` 里 `StopTransceiver` | ❌ 不需要手动 removeTrack/stop，只需最后释放引用 |
| **DataChannel** | `Close()` 拆 transport 时自动 close 全部 | ❌ 不需要逐个 `channel->Close()` |
| **媒体管线（ADM + capturer）** | **没人替你停** | ✅ **必须显式停** |

- DataChannel 自动关闭的依据：[data_channel_controller.cc:228-243](pc/data_channel_controller.cc#L228-L243)，`OnTransportClosed()` 注释原文 *"This loop will close all data channels"*。
- 所以 **`Close()` 会自动关掉所有 DataChannel**，不需要手动逐个关。

---

## 五、正确的挂断顺序

> ⚠️ 常见误区：以为要"先删 track / channel → 再删媒体管线 → 再删 PC"。
> **实际上顺序要反过来**：先停物理管线 → `Close()` 一次搞定 track+channel → 最后释放引用。

```cpp
// ① 先停物理媒体管线（Close() 不会做这步）
adm->StopRecording();        // 停麦克风
adm->StopPlayout();          // 停扬声器
video_capturer->Stop();      // 停摄像头（或释放 video source 引用触发析构）

// ② Close() 一次性搞定 RTP 层：停所有 sender/receiver + 关所有 data channel
peer_connection_->Close();

// ③ 释放所有引用（真正"销毁"对象）
audio_track_          = nullptr;   // 释放 track 引用
video_track_          = nullptr;
data_channel_         = nullptr;   // 释放 channel 引用
local_video_source_   = nullptr;   // 释放视频源
peer_connection_      = nullptr;   // 释放 PC
peer_connection_factory_ = nullptr; // 释放 factory（若 ADM 归它管，此时才真正停）
```

### 为什么"先删 track / channel"是多余甚至有害的

1. **多余**：`Close()` 内部已经会 stop 所有 transceiver、close 所有 channel，再手动逐个删是重复劳动。
2. **有害**：PC 还活着时对 sender 调 `RemoveTrack()`，会触发 **renegotiation（重新协商）** 语义，产生新的 offer/answer 流程，而挂断根本不该触发协商。
3. **引用关系**：`AddTrack` 后，PC 内部的 sender 也持有 track 的引用。你即使先 `track_ = nullptr`，它也不会真销毁（sender 还抓着）。必须等 `Close()` 让 sender 停止、释放引用后，你的引用释放才真正让它归零。

官方 canonical 例子的释放参考 [conductor.cc:262-270](examples/peerconnection/client/conductor.cc#L262-L270)：

```cpp
void Conductor::DeletePeerConnection() {
  main_wnd_->StopLocalRenderer();    // 先停渲染
  main_wnd_->StopRemoteRenderer();
  peer_connection_ = nullptr;        // 释放 PC（析构内部 Close）
  peer_connection_factory_ = nullptr; // 释放 factory（→ ADM 停止）
  local_video_source_ = nullptr;      // 释放视频源（→ capturer 停止）
  peer_id_ = -1;
  loopback_ = false;
}
```

---

## 六、重连：要重建什么

| 对象 | 重连时 | 说明 |
|------|-------|------|
| **PeerConnection** | 重建 | `Close()` 不可逆，必须新建 |
| **Audio/Video Track** | 重建 | 新建 source → 新建 track → `AddTrack` |
| **DataChannel** | 重建 | 重新 `CreateDataChannel` |
| **媒体管线（capturer）** | 重建 | 新建 capturer → Start |
| **PeerConnectionFactory** | **可复用** | 除非 ADM 归 factory 管且把 factory 也销毁了，否则不必重建 |

最小重建：**复用 factory，只重建 PC + track + channel + capturer**。

### "重连"分两种场景，处理方式不同（重要）

| 场景 | 原因 | 正确做法 |
|------|------|---------|
| **A. 同一 peer 的临时断网**（ICE `disconnected`/`failed`） | 连接还在，只是网络抖动 | **不需要重建**，用 **ICE restart**（`createOffer({ iceRestart: true })`） |
| **B. 真正挂断 / 对端刷新 / 换 peer / 新会话** | 连接已彻底关闭 | **必须重建新的 PeerConnection**，重走完整 SDP/ICE 握手 |

依据：WebRTC 专家 Philipp Hancke 的回答 —— 场景 B（对端刷新页面）时，旧对象已丢失 DTLS 凭据，无法复用旧 candidate / localDescription，两端都必须重建。

---

## 七、一句话总结

| 问题 | 答案 |
|------|------|
| 挂断先 `Close()` 再销毁 PC？ | ✅ 对，`Close()` 不可逆，之后必须释放 |
| 重连要重新 `CreatePeerConnection`？ | ✅ 对，closed 是终态，不能复用 |
| Factory 也要重建吗？ | ❌ 不必需，可复用 |
| track / channel 要手动先删吗？ | ❌ 不需要，`Close()` 自动 stop/close |
| 媒体管线（麦克风/摄像头）要停吗？ | ✅ **要，且必须显式做**，`Close()` 不管它 |
| 正确顺序？ | ① 停媒体管线 → ② `Close()` → ③ 释放所有引用 |

**核心一句话：`Close()` 管 RTP 层（track + channel 全包），唯独不管物理采集层（媒体管线要自己停）。**

---

## 附：参考资料

**源码（本 checkout）**
- `api/peer_connection_interface.h` — `Close()` 声明与注释、`StatsObserver`、`PeerConnectionObserver`
- `pc/peer_connection.cc` — `Close()` / `~PeerConnection()` 实现
- `pc/data_channel_controller.cc` — `OnTransportClosed()` 关闭所有 data channel
- `media/engine/webrtc_voice_engine.cc` — `Terminate()` 停止 ADM
- `examples/peerconnection/client/conductor.cc` — 官方例子的 `DeletePeerConnection()`、`CapturerTrackSource`

**网络资料**
- [MDN — RTCPeerConnection: close() method](https://developer.mozilla.org/en-US/docs/Web/API/RTCPeerConnection/close)
- [Stack Overflow — WebRTC: Reusing connections](https://stackoverflow.com/questions/44188177/webrtc-reusing-connections)
- [Stack Overflow — Change connectionState from disconnected to new](https://stackoverflow.com/questions/67779757/change-rtcpeerconnections-change-connectionstate-from-disconnect-to-new)
- [WebRTC Samples — ICE restart](https://webrtc.github.io/samples/src/content/peerconnection/restart-ice/)
- [daily.dev — Designing Ephemeral Video Chat Sessions with WebRTC](https://daily.dev/posts/designing-ephemeral-video-chat-sessions-with-webrtc-and-javascript-9avi8wcbx)
- [CSDN — RTCPeerConnection close 后资源未释放？](https://ask.csdn.net/questions/9016512)
- [Chromium Issue — StopStream for the local media stream should stop the capturer](https://codereview.chromium.org/12526011)
- [WebRTC PeerConnection 资源释放（native）](https://www.e-com-net.com/article/1695636021166813184.htm)
