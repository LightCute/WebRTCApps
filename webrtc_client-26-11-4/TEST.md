# webrtc_engine 客户端测试报告

日期：2026-11-05
状态：全部通过

模块：`webrtc_engine`（经典三线程 WebRTC 客户端：signaling / worker / network），
上层通过 unix_socket（`ipc_test26_10_26` 的 `UnixSocketServer` + `EngineController`）交互。
命令方向为纯文本，事件方向为 JSON（每行一个对象）。

构建：`bash build_x64.sh` → 产物在 `out/webrtc_client_x64/`。

## 1. 冒烟测试 `webrtc_engine_smoke`

```bash
out/webrtc_client_x64/webrtc_engine_smoke
```

| 覆盖内容 | 结果 |
|---|---|
| 三线程启动 + `PeerConnectionFactory` + `PeerConnection` + `DataChannel` 创建，断言各「已创建」标志 | 通过 |
| 反复创建/销毁 20 次（含工厂/PC/DC），验证析构顺序安全、无泄漏崩溃 | 通过 |

输出：`webrtc_engine_smoke: all pass`。

## 2. 信令集成测试 `webrtc_engine_signaling_test`

```bash
bash test/integration_test.sh   # 起真信令服务器(9799) + 本测试二进制
```

| 覆盖内容 | 结果 |
|---|---|
| 连真信令服务器并 `signed_in`，`my_id` 有效 | 通过 |
| 回调确实运行在 signaling 线程（`callback_on_signaling_thread`） | 通过 |

输出：`signaling integration ok: my_id=1`。

## 3. 通话集成测试 `webrtc_engine_call_test`

```bash
bash test/call_integration_test.sh   # 起真信令服务器(9899) + 本测试二进制
```

进程内起 offerer / answerer 两个引擎，走完整 offer/answer 闭环：

| 覆盖内容 | 结果 |
|---|---|
| 双方注册 → `call` → offer/answer + candidate → PC `connected` | 通过 |
| `DataChannel` 双端 `open` | 通过 |
| `SendDataChannelMessage("hello")` → 对端 `last_received_message() == "hello"` | 通过 |

输出：`call integration ok: connected, DataChannel open, message delivered`。

## 4. 挂断 / 掉线清理测试 `webrtc_engine_teardown_test`

```bash
bash test/teardown_integration_test.sh   # 起真信令服务器(9898) + 本测试二进制
```

| 场景 | 覆盖内容 | 结果 |
|---|---|---|
| 通话中 A 主动 `leave` | 双方 `is_connected()==false` 且 PC 已清理（`is_peer_connection_created()==false`） | 通过 |
| 通话中 A 掉线（断信令） | B 收到 `peer_gone`，`is_connected()==false` 且 PC 已清理 | 通过 |

输出：`leave cleanup ok` / `peer_gone cleanup ok` / `teardown integration ok`。

## 5. 多轮通话测试 `webrtc_engine_multicall_test`

```bash
bash test/multicall_integration_test.sh   # 起真信令服务器(9897) + 本测试二进制
```

登录一次，重复 20 次：`call` → PC connected → DC open → 发消息（每轮消息不同）→ `leave` → 双方清理。

| 覆盖内容 | 结果 |
|---|---|
| 20 轮通话/挂断，无状态残留、无崩溃 | 通过 |

输出：`multicall integration ok: 20 rounds`。

## 6. IPC 集成测试 `ipc_integration_test.py`

```bash
python3 test/ipc_integration_test.py
```

起真信令服务器(9896) + 两个 `webrtc_engine_main` 进程，各连各的 unix_socket，
用文本命令驱动、按 JSON 事件断言：

| 覆盖内容 | 结果 |
|---|---|
| 连接成功收到握手 `{"event":"ipc.connected","commands":[...]}`，命令清单含全部 10 条 | 通过 |
| `register` → `signed_in`（id=1/2） | 通过 |
| `get_roster` → roster 带两人且含 `state`（idle） | 通过 |
| `peer_connection_status` → `signed_in=true` | 通过 |
| `call 2` → 双方 `peer_connection.state=connected` + 对端 `data_channel.state=open` | 通过 |
| `data_channel_msg hello-from-ipc-test` → 对端 `data_channel_msg.text` 一致 | 通过 |
| `get_stats` → `stats` 快照（data_channel 状态/计数、transport、candidate_pair RTT） | 通过 |
| `leave` → 双方 `peer_connection.state=closed` | 通过 |
| `quit` → 两个引擎进程正常退出 | 通过 |

输出：`ipc integration ok: register/call/msg/leave/quit via unix_socket (json)`。

## 7. IPC 多轮通话测试 `ipc_multicall_test.py`

```bash
python3 test/ipc_multicall_test.py
```

起真信令服务器(9895) + 两个 `webrtc_engine_main`，unix_socket 登录一次后，
重复 20 次：`call` → connected → DC open → `data_channel_msg`（每轮不同）→ `leave` → closed。

| 覆盖内容 | 结果 |
|---|---|
| 登录一次，20 轮通话/挂断走 IPC，无残留、无崩溃 | 通过 |

输出：`ipc multicall ok: 20 rounds (login once)`。

## 8. IPC 信令控制测试 `ipc_signaling_test.py`

```bash
python3 test/ipc_signaling_test.py
```

起两个信令服务器(9893/9892) + 两个 `webrtc_engine_main`，覆盖无需通话的信令控制与转发：

| 覆盖内容 | 结果 |
|---|---|
| `register <name> <ip> <port>` 显式指定服务器 | 通过 |
| `peer_connection_status`（空闲）→ `signed_in=true`、`connected=false` | 通过 |
| `signal_msg <peer_id> <text>` 经信令服务器转发（无通话） | 通过 |
| `signal_msg <离线id>` → `signal_client.error` code=offline | 通过 |
| `register` 切换服务器 | 通过 |
| `signal_disconnect` → `disconnected` 且状态清理 | 通过 |
| 断开后 `register` 重连 | 通过 |

输出：`ipc signaling ok: register(ip/port)/switch/disconnect/reconnect/status/signal_msg`。

## 9. IPC 断开清理测试 `ipc_disconnect_test.py`

```bash
python3 test/ipc_disconnect_test.py
```

起真信令服务器(9891) + 两个 `webrtc_engine_main`，覆盖通话中的信令断开清理：

| 覆盖内容 | 结果 |
|---|---|
| 通话中 `peer_connection_status` → `connected=true`、`data_channel_open=true` | 通过 |
| 通话中 `signal_disconnect` → 本端 disconnected + 双方 `peer_connection.state=closed` | 通过 |
| 通话中 kill 信令服务器 → 双方 disconnected + `peer_connection.state=closed` | 通过 |

输出：`ipc disconnect ok: signal_disconnect + server crash teardown`。

## 10. 受理校验 + engine.error 单测 `webrtc_engine_validation_test`

```bash
out/webrtc_client_x64/webrtc_engine_validation_test
```

| 覆盖内容 | 结果 |
|---|---|
| 受理校验谓词：未注册时 `CanStartCall/CanGetRoster/CanLeave` 返回 `not_signed_in` | 通过 |
| bool 语义：未建 DC `SendDataChannelMessage` 返回 false；未连服务器 `SendSignalMessage` 返回 false | 通过 |
| engine.error：无 factory 建 PC → `peer_connection_failed`；无 PC 建 DC → `data_channel_failed`；非法远端 offer → `set_remote_desc_failed` | 通过 |

输出：`validation ok: predicates + engine.error`。

## 11. 命令失败返回 IPC 测试 `ipc_failure_test.py`

```bash
python3 test/ipc_failure_test.py
```

起真信令服务器(9890) + 两个 `webrtc_engine_main`，未注册/未通话状态下发命令，断言 `ipc.error` 错误码：

| 覆盖内容 | 结果 |
|---|---|
| 未通话 `data_channel_msg` → `data_channel_not_open` | 通过 |
| 未注册 `signal_msg` → `signal_send_failed` | 通过 |
| 未注册 `get_roster`/`leave`/`call` → `not_signed_in` | 通过 |
| 通话中再 `call` → `already_in_call` | 通过 |

输出：`ipc failure ok: data_channel_not_open/signal_send_failed/not_signed_in/already_in_call`。

## 说明

- 集成测试均依赖真 `signal_server_26_11_3/build/signaling_server`，各测试使用独立端口（9799/9899/9898/9897/9896/9895/9893/9892/9891/9890），避免互扰。
- IPC 层已纳入断言的改动：连接握手 + 命令清单、roster 带在线用户状态、`register` 指定/切换服务器、`signal_disconnect`/崩溃拆 PC、信令转发 `signal_msg`、运行时状态快照 `get_stats`。
- 错误事件分三类 scope：`ipc.error`（受理拒绝）、`engine.error`（引擎内部异步失败）、`signal_client.error`（信令服务器返回），完整命令 ↔ 事件/错误码汇总见 [IPC_PROTOCOL.md](IPC_PROTOCOL.md)。
- `get_stats` 返回的 `stats` 事件含三块：`data_channel`（状态/计数/积压，同步采集）、`transport` + `candidate_pair`（字节/DTLS/ICE/RTT，来自 PeerConnection GetStats）。
- 事件协议（后端 → 上层）为 JSON，字段以 `event` 路由；命令（上层 → 后端）保持纯文本。
