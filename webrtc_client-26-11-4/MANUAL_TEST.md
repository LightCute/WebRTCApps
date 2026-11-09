# 手动测试手册（webrtc_engine_main 后端）

本文记录如何**手动**启动 `webrtc_engine_main` 后端，通过 unix socket 走 IPC 协议驱动它，
完成一次跨 NAT 的通话/断开测试。协议细节见 [IPC_PROTOCOL.md](IPC_PROTOCOL.md)。

## 1. 前置准备

| 项 | 本地 | 云端 |
|---|---|---|
| 后端二进制 | `out/webrtc_client_x64/webrtc_engine_main` | `/root/stun_test/webrtc_engine_main` |
| `.env` 配置 | [.env](.env)（`WEBRTC_SERVER=120.79.210.6`） | `/root/stun_test/.env`（`WEBRTC_SERVER=127.0.0.1`） |
| 信令服务器 | — | 云端 `8888`（需已在运行） |

`.env` 内容（关键键）：

```ini
WEBRTC_SERVER=120.79.210.6      # 云端 daemon 改成 127.0.0.1
WEBRTC_SERVER_PORT=8888
WEBRTC_STUN=stun:stun.l.google.com:19302
WEBRTC_TURN_SERVER=turn:120.79.210.6:3478
WEBRTC_TURN_USER=test
WEBRTC_TURN_PASSWORD=123
ICE_TRANSPORTS=srflx           # auto / host / srflx / relay
```

## 2. 连接云服务器

```bash
ssh root@120.79.210.6
```

## 3. 后台运行后端

**本地（offerer）**：

```bash
cd /home/light/webrtc/checkout/src/out/webrtc_client_x64
./webrtc_engine_main \
  --socket_path=/tmp/wc_local.sock \
  --env_file=/home/light/webrtc/checkout/src/apps/webrtc_client-26-11-4/.env &
```

**云端（answerer）**：

```bash
ssh root@120.79.210.6 'cd /root/stun_test && nohup ./webrtc_engine_main \
  --socket_path=/tmp/wc_cloud.sock \
  --env_file=/root/stun_test/.env > /tmp/wc_cloud.log 2>&1 &'
```

> `--socket_path` 指定 unix socket 路径；`--log_dir` 指定日志目录（默认 `/tmp/webrtc_engine`）。
> 重启前先 `rm -f <socket_path>` 清掉残留 socket。

## 4. 连接 unix socket

```bash
# 本地后端
nc -U /tmp/wc_local.sock

# 云端后端（从本地机连）
ssh root@120.79.210.6 'nc -U /tmp/wc_cloud.sock'
```

> 连上后后端先回一行 `ipc.connected`（含全部命令清单），之后一行一条发命令、一行一个 JSON 事件。

## 5. 命令速查（前端 → 后端）

| 命令 | 作用 | 返回事件 |
|---|---|---|
| `register <name> [ip] [port]` | 注册到信令服务器 | `signal_client.signed_in`（id） |
| `get_roster` | 查在线列表 | `signal_client.roster`（peers: id/name/state） |
| `call <peer_id>` | 呼叫对端 | `peer_connection.state`=connected → `data_channel.state`=open |
| `peer_connection_status` | 同步查状态 | `peer_connection_status`（signed_in/my_id/connected/...） |
| `leave` | 挂断 | `signal_client.call_ended` |
| `signal_disconnect` | 断信令（保留进程可重连） | `signal_client.disconnected` |
| `data_channel_msg <text>` | 发数据消息 | — |
| `signal_msg <peer_id> <text>` | 发信令文本 | — |
| `get_stats` | 采集运行时状态 | `stats` |
| `quit` | 退出后端进程 | — |

## 6. 手把手：一次手动通话

**终端 A（本地 offerer）**：

```
$ nc -U /tmp/wc_local.sock
{"event":"ipc.connected","commands":[...]}
register offerer 120.79.210.6 8888
{"event":"signal_client.signed_in","id":19}
get_roster
{"event":"signal_client.roster","peers":[{"id":20,"name":"answerer","state":"idle"},{"id":19,"name":"offerer","state":"idle"}]}
call 20                                   ← answerer 的 id
{"event":"peer_connection.state","state":"connected"}
{"event":"data_channel.state","state":"open"}
leave
{"event":"signal_client.call_ended","from":20}
quit
```

**终端 B（云端 answerer）**：

```
$ ssh root@120.79.210.6 'nc -U /tmp/wc_cloud.sock'
{"event":"ipc.connected","commands":[...]}
register answerer 127.0.0.1 8888
{"event":"signal_client.signed_in","id":20}
{"event":"signal_client.incoming_call","from":19}     ← 收到来电（自动应答）
{"event":"peer_connection.state","state":"connected"}
{"event":"data_channel.state","state":"open"}
{"event":"signal_client.call_ended","from":19}        ← 对方挂断
quit
```

> 注意：`call` 要传**数字 id**（从 `get_roster` 拿到），不是名字；id 每次注册递增，别写死。

## 7. 已知行为（bug 修复后）

- 连接**失败**（`peer_connection.state`=failed）时，后端会自动发 `leave` 清掉信令服务器的
  `busy` 状态并通知双方清理（不再一直卡 busy）。
- `signal_disconnect` 后可再 `register` 重连（软断，保留进程）。
