# WebSocket 信令客户端（peer_connection_client_26_11_3）

WebRTC 引擎要用的 WebSocket 信令客户端，**直接收发线上协议**（register/call/answer/
candidate/leave/get_roster），不做任何翻译。连到新信令服务器（`signal_server_26_11_3`）。

## 编译

```bash
bash build.sh
```

产物（在 `build/`）：

| 文件 | 说明 |
|---|---|
| `build/client_smoke` | 构造/析构冒烟 |
| `build/client_harness` | 集成测试 harness（连服务器 + 自动回 answer + 打印事件） |

## 目录结构

```
ws_signaling_client.h/.cc   # WebSocketSignalingClient + SignalingClientObserver
test/                       # harness + smoke + 集成测试
build/                      # 编译产物
```

## 核心接口

```cpp
// 回调（类型化，直接对应线上协议）
struct SignalingClientObserver {
  OnSignedIn(my_id) / OnDisconnected() / OnRoster(peers)
  OnCall(from, sdp) / OnAnswer(from, sdp) / OnCandidate(from, cand, mid, idx)
  OnLeave(from) / OnPeerGone(from) / OnError(code, msg) / OnMessage(from, text)
};

class WebSocketSignalingClient {
  Connect(server, port, name) / Close()
  Call(peer, sdp) / Answer(peer, sdp) / Candidate(peer, cand, mid, idx)
  Leave() / GetRoster() / SendMessage(peer, text)
  is_connected() / id() / RegisterObserver() / SetCallbackExecutor()
};
```

- 内部一个 io 线程跑 websocketpp 的 `run()`，所有 handler 在 io 线程触发。
- `SetCallbackExecutor` 注入执行器，把回调 marshal 回引擎线程（如 `rtc::Thread::PostTask`）。
- 纯 websocketpp + standalone Asio + jsoncpp + std::thread，零 rtc_base。

## 集成测试

```bash
bash test/integration_test.sh
```

起真 `signal_server` + 真 `test_client` 当对端 + 本目录 `client_harness`，跑完整流程：
harness 注册 → peer 注册 → peer `call` harness → harness 自动 `answer` → peer 发 candidate →
peer `leave` → harness 收到 `OnLeave`。

预期 harness 打印：

```
EVENT signed_in id=1
EVENT roster 1:harness
EVENT roster 1:harness 2:robot
EVENT call from=2
EVENT candidate from=2
EVENT leave from=2
```

## 说明

- 客户端是「薄传输层」：只把线上 JSON 转成类型化回调，roster 原样下发（含自己，由上层过滤）。
- 引擎侧将重构为直接使用本接口（`Call/Answer/Candidate/Leave/GetRoster`），不再有 `SendToPeer`
  通用消息与 `offer↔call` 翻译。
- 回调 marshal（`SetCallbackExecutor`）机制已实现，但尚未在真 WebRTC 线程上端到端验证。
