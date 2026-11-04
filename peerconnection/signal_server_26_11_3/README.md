# WebSocket 信令服务器（signal_server_26_11_3）

最小 WebSocket 信令服务器（websocketpp + standalone Asio 1.24 + jsoncpp），用于任意两客户端
之间的 1v1 通话信令：注册 / 在线列表 / 呼叫 / SDP / ICE / 忙线 / 离开 / 心跳。

## 编译

```bash
bash build.sh
```

产物：

| 文件 | 说明 |
|---|---|
| `build/signaling_server` | 信令服务器 |
| `build/test_client` | 最小测试客户端（说同一套 JSON 协议） |
| `build/messages_test` | 消息编解码单元测试 |
| `build/registry_test` | 注册表状态机单元测试 |
| `build/logger_test` | 日志模块单元测试（含滚动） |

## 启动服务器

```bash
./build/signaling_server --port=8888 --ping_interval=10 --ping_timeout=30 \
    --log_file=server.log --log_max_size_mb=10 --log_max_files=3
```

参数均可选：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port` | 8888 | 监听端口 |
| `--ping_interval` | 10 | 心跳秒数（WebSocket ping/pong） |
| `--ping_timeout` | 30 | 心跳超时秒数，超时判定掉线 |
| `--log_file` | 空 | 日志文件路径；空 = 只打印到 stdout |
| `--log_max_size_mb` | 10 | 日志文件滚动阈值（MB） |
| `--log_max_files` | 3 | 滚动保留的历史文件份数 |

日志同时打印到 stdout 并（若指定 `--log_file`）写入文件，格式
`[年月日时分秒.毫秒] [级别] 消息`。

## 测试客户端

```bash
./build/test_client ws://127.0.0.1:8888
```

命令：

```
register <name>                    # 上线登记（自报名字，服务器分配临时 id）
call <id> <sdp>                    # 呼叫（主叫先造 offer 一起带上）
answer <id> <sdp>                  # 回 answer（自动接听，无拒绝环节）
candidate <id> <cand> <mid> <idx>  # ICE 候选
msg <id> <text>                    # 发文本给指定客户端（text 为剩余整行）
leave                              # 离开通话（不带 id，服务器反查对端）
get_roster                         # 主动查询当前在线列表
reconnect                          # 断开并重新连接服务器（重连后需重新 register）
quit                               # 退出
```

服务器下发的消息以 `<< ` 前缀打印。

## 消息协议

JSON 文本帧，`type` 区分消息。客户端→服务器：`register` / `call` / `answer` /
`candidate` / `leave` / `get_roster` / `message`；服务器→客户端：`welcome` / `error` /
`roster` / `call` / `answer` / `candidate` / `leave` / `peer_gone` / `message`。

- 转发消息由服务器补 `from`；`answer`/`candidate` 原样透传。
- `leave` 不带 id：1v1 下客户端最多一个对端，服务器从连接反查对端后通知双方、清状态。
- `get_roster` 只回给发起查询的客户端，不广播。
- 心跳走 WebSocket 自带 ping/pong，客户端协议栈自动回 pong，无需客户端额外代码。

详见设计文档 `docs/superpowers/specs/2026-11-03-websocket-signaling-server-design.md` §5。

## 集成验证步骤

开两/三个终端，一个跑服务器，其余跑 `test_client`：

1. **完整通话闭环**：A `register robot`、B `register monitor`；A `call 2 offer-sdp`；
   B `answer 1 answer-sdp`、`candidate 1 cand 0 0`；A `leave`。
   预期：双方按序收到 welcome/roster/call/answer/candidate/leave，leave 后 roster 回 idle。
2. **查询在线列表**：任意客户端 `get_roster`。预期：收到当前 `roster`。
3. **忙线拒呼**：A、B 通话中，C `register cam` 后 `call 1 x`。预期：C 收到 `error busy`。
4. **名字撞车**：新客户端 `register` 已在线名字。预期：收到 `error name_taken`。
5. **通话中掉线**：A、B 通话中强杀 B。预期：A 收到 `peer_gone`，roster 中 B 消失、A 回 idle。
6. **重复注册**：已注册客户端再 `register`。预期：被忽略，不产生新 id、不重复 welcome。
