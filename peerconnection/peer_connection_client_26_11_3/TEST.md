# WebSocket 信令客户端测试报告

日期：2026-11-04
状态：全部通过

## 1. 单元测试

```bash
bash build.sh
./build/dispatch_test
```

| 测试 | 覆盖内容 | 结果 |
|---|---|---|
| `dispatch_test` | 「线上 JSON → 回调」分发：`welcome`/`roster`/`call`/`answer`/`candidate`/`leave`/`peer_gone`/`message`/`error` 九种消息，直接喂 JSON 断言 `RecordingObserver` 收到正确回调及字段 | 通过 |

`dispatch_test` 的做法：`DispatchMessage` 是 `protected virtual`，测试用子类 `using`
暴露后直接调用，不连网、不起 io 线程。

## 2. 冒烟测试

| 测试 | 内容 | 结果 |
|---|---|---|
| `client_smoke` | 构造/析构 `WebSocketSignalingClient` | 通过 |

## 3. 集成测试

```bash
bash test/integration_test.sh
```

起真 `signal_server` + 真 `test_client` 当对端 + 本目录 `client_harness`，跑完整通话闭环：

| 场景 | 预期 | 结果 |
|---|---|---|
| 完整通话闭环 | harness 注册 → peer 注册 → peer `call` → harness 自动 `answer` → `candidate` → peer `leave` → harness 收 `OnLeave` | 通过 |
| 文本 relay | peer `msg` → 服务器转发 → harness 收 `OnMessage` | 通过 |

## 4. 说明

- 早期版本的 `message_translator_test`、`roster_test` 随翻译层（`offer↔call` 映射）一起移除。
- 回调 marshal（`SetCallbackExecutor` 切到引擎线程）机制已实现，但尚未在真 WebRTC 线程上端到端验证。
