# learning 通信流程与观察者详解

本文讲解 `learning` 工程里 WebRTC DataChannel 建连的完整流程，以及**每个观察者类在什么时候被通知**。

代码位置：`learning_common.cc`（共享实现，offer/answer 两个二进制共用）。

## 1. 观察者总览

`learning` 里共有 **6 个观察者类**，分别盯住不同的对象：

| 观察者类 | 继承的接口 | 盯的对象 | 关键回调 |
|---|---|---|---|
| `IpcObserver` | `UnixSocketObserver` | Unix socket（IPC 层） | `OnClientConnected` / `OnLineReceived` / `OnClientDisconnected` |
| `PeerConnectionObserverImpl` | `PeerConnectionObserver` | PeerConnection | `OnSignalingChange` / `OnDataChannel` / `OnIceGatheringChange` / `OnIceCandidate` / `OnConnectionChange` |
| `DcdataObserver` | `DataChannelObserver` | DataChannel | `OnStateChange` / `OnMessage` |
| `DcCreateSdpObserver` | `CreateSessionDescriptionObserver` | CreateOffer / CreateAnswer | `OnSuccess` / `OnFailure` |
| `DcSetLocalObserver` | `SetLocalDescriptionObserverInterface` | SetLocalDescription | `OnSetLocalDescriptionComplete` |
| `DcSetRemoteObserver` | `SetRemoteDescriptionObserverInterface` | SetRemoteDescription | `OnSetRemoteDescriptionComplete` |

## 2. 线程归属

| 线程 | 上跑什么 |
|---|---|
| **io 线程**（`UnixSocketServer` 内部的 epoll 线程） | `IpcObserver` 的三个回调。里面只做 `g_thread->PostTask(...)` 转投，快速返回 |
| **g_thread**（WebRTC 线程，signaling/network/worker 三合一） | 其余 5 个 WebRTC 观察者的回调 + 所有状态机逻辑 |

关键：所有 WebRTC 观察者回调都在 **g_thread** 上串行执行；IpcObserver 回调先落在 io 线程，再 PostTask 到 g_thread。

## 3. offerer 完整时间线

前置：`RunSession()` 里创建 factory + PC，`g_ipc_server->Start()` + `g_thread->Run()` 后进入等待。此时没有任何观察者被通知。

1. **客户端（socat）连上**
   - io 线程 accept → `IpcObserver::OnClientConnected(client_id)` 被通知。
   - PostTask 到 g_thread：记 `g_client_id`、`g_phase=kAwaitSdp`，调 `StartOffer()`。
   - `StartOffer()`：`CreateDataChannelOrError("chat")`（offerer 主动建 DC，并 `RegisterObserver(&g_data_observer)`）+ `CreateOffer(g_sdp_observer)`。

2. **offer SDP 生成完**
   - `DcCreateSdpObserver::OnSuccess(desc)` 被通知。转成字符串 → `SetLocalDescription(...)` → 把 offer SDP + 空行发给客户端。

3. **本地描述设置完**
   - `DcSetLocalObserver::OnSetLocalDescriptionComplete(error)` 被通知（`SetLocalDesc OK`）。

4. **信令状态变化**
   - `PeerConnectionObserverImpl::OnSignalingChange(kHaveLocalOffer)` 被通知。

5. **ICE 采集**
   - `OnIceGatheringChange(kIceGatheringGathering)` 被通知。
   - 每采集到一个候选 → `OnIceCandidate(candidate)` 被通知一次（多次），存进 `g_local_candidates`。
   - 采完 → `OnIceGatheringChange(kIceGatheringComplete)` 被通知 → `MaybeSendLocalCandidates()`（此刻 phase 仍是 `kAwaitSdp`，不发）。

6. **人把 answer SDP 粘贴进来**
   - io 线程每收到一行 → `IpcObserver::OnLineReceived(id, line)` 被通知一次 → PostTask → `HandleLine` 累积。
   - 空行 → `HandleLine` 组装 answer，`SetRemoteDescription(kAnswer)`，`g_phase` 切到 `kAwaitCandidates`，`MaybeSendLocalCandidates()`（ICE 已完则发候选 + `done`）。

7. **远端描述设置完**
   - `DcSetRemoteObserver::OnSetRemoteDescriptionComplete(error)` 被通知（offerer 只打日志）。

8. **人把 answerer 的候选粘贴进来**
   - `OnLineReceived` 逐行 → `HandleLine`（kAwaitCandidates）→ `AddRemoteCandidate`（`AddIceCandidate`），直到 `done`。

9. **ICE 连接建立**
   - `OnConnectionChange(kConnecting)` → `OnConnectionChange(kConnected)` 被通知。kConnected 时发 `>>> CONNECTION ESTABLISHED <<<`。

10. **DataChannel 打开**
    - `DcdataObserver::OnStateChange(kOpen)` 被通知（`IsOkToCallOnTheNetworkThread()==true`，跑 network 线程 = g_thread）。发 `DataChannel OPEN — ready to chat!`。

11. **聊天**
    - 人输入一行 → `OnLineReceived` → `HandleLine`（`g_dc->state()==kOpen`）→ `g_dc->Send`。
    - 对端发来消息 → `DcdataObserver::OnMessage(buffer)` 被通知 → 发 `Peer: xxx`。

12. **挂断**
    - 人输 `/quit` → `HandleLine` → `Hangup()`（发 `/bye`）→ `Teardown()`。
    - 对端收到 `/bye` → `OnMessage` → `Teardown()`。
    - `Close` 过程中 `OnStateChange(kClosing/kClosed)` 也会被通知，但 `g_shutting_down` 防重入。

## 4. answerer 完整时间线

answerer 与 offerer 大体对称，差异在**谁先发 SDP、谁收 DataChannel**。

1. **客户端连上**
   - `IpcObserver::OnClientConnected` 被通知 → 记 id、`g_phase=kAwaitSdp`，**不调 StartOffer**，只等 offer。

2. **人把 offer SDP 粘贴进来**
   - `OnLineReceived` 逐行 → `HandleLine` 累积，空行 → `SetRemoteDescription(kOffer)`。

3. **远端 offer 设置完**
   - `DcSetRemoteObserver::OnSetRemoteDescriptionComplete(error)` 被通知。answerer 角色下它**紧接着调 `CreateAnswer()`**。

4. **answer SDP 生成完**
   - `DcCreateSdpObserver::OnSuccess(desc)` 被通知 → `SetLocalDescription` + 发 answer SDP + 空行；answerer 角色下 `g_phase` 切到 `kAwaitCandidates`，并 `MaybeSendLocalCandidates()`。

5. **本地描述设置完**
   - `DcSetLocalObserver::OnSetLocalDescriptionComplete` 被通知。

6. **信令状态变化**
   - `OnSignalingChange(kHaveRemoteOffer)` → `OnSignalingChange(kStable)` 先后被通知。

7. **ICE 采集**
   - `OnIceGatheringChange(gathering)` → `OnIceCandidate` ×N → `OnIceGatheringChange(complete)` → `MaybeSendLocalCandidates()`（此刻已是 kAwaitCandidates → 发候选 + `done`）。

8. **人把 offerer 的候选粘贴进来**
   - `OnLineReceived` → `AddRemoteCandidate`，直到 `done`。

9. **ICE 连接**
   - `OnConnectionChange(kConnecting → kConnected)` 被通知。

10. **收到对端 DataChannel（answerer 特有）**
    - `PeerConnectionObserverImpl::OnDataChannel(data_channel)` 被通知 → `g_dc = data_channel` + `RegisterObserver(&g_data_observer)`。
    - 这是 offerer 在 `CreateDataChannel` 时建的 channel 通过 SCTP 送过来时触发的（in-band 协商）。

11. **DataChannel 打开**
    - `DcdataObserver::OnStateChange(kOpen)` 被通知。

12. **聊天 / 挂断** 与 offerer 完全一致。

## 5. 通知链总结

```
人/网络做了什么
   │
   ├─ Unix socket 有数据 ──→ IpcObserver（io 线程）──PostTask──→ 状态机 HandleLine
   │
   ├─ 生成 offer/answer ────→ DcCreateSdpObserver::OnSuccess
   ├─ 设置本地描述 ─────────→ DcSetLocalObserver::OnSetLocalDescriptionComplete
   ├─ 设置远端描述 ─────────→ DcSetRemoteObserver::OnSetRemoteDescriptionComplete
   │                        （answerer 在这里再触发 CreateAnswer）
   ├─ 信令/ICE/连接状态 ────→ PeerConnectionObserverImpl 的 5 个 OnXxx
   │                        （OnIceCandidate 收集候选，OnConnectionChange 判定连通）
   └─ DataChannel 状态/数据 → DcdataObserver::OnStateChange / OnMessage
```

## 6. 核心心智模型

WebRTC 是**纯异步事件驱动**的：调 `CreateOffer` / `SetLocalDescription` / `AddIceCandidate` 只是"发起"，真正的结果和推进靠上面这些观察者回调一步步接力。

- **长期观察者**（`IpcObserver`、`PeerConnectionObserverImpl`、`DcdataObserver`）：跟随对象生命周期，持续接收事件。
- **一次性观察者**（`DcCreateSdpObserver`、`DcSetLocalObserver`、`DcSetRemoteObserver`）：作为参数传给某个异步方法，操作完成后回调一次即弃。

理解"哪个回调在哪个时刻被通知"，就等于理解了整条建连链路。
