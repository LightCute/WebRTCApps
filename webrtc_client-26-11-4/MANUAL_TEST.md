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
| `set_ice_transports <mode>` | 空闲时改下次 call 的 ICE 模式 | `ice_config` |
| `switch_ice <mode>` | 通话中切本端 ICE 模式（重建 PC 重连） | `ice_switched`（success/failed） |
| `reconnect` | 通话中 ICE 重启（复用旧候选，不改 mode） | `reconnected`（success/failed） |
| `get_ice_config` | 查当前 ICE 配置 | `ice_config`（mode/stun/turn） |
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

## 8. 通话中切换 ICE 模式 / 重连（流畅性测试）

### 语义（先搞清楚，否则容易误判）

- **mode 是每端本地的**：`switch_ice <mode>` 只切换**被命令的那端**，对端 mode 不变。
- `switch_ice <mode>` = **重建 PeerConnection**：释放旧候选（旧 TURN 分配）→ 用新 mode
  重新采集 → 重新 offer/answer。候选类型变了，所以必须重建。
- `reconnect` = **同 PC 上 ICE 重启**：复用旧候选（新 ufrag）、不重建、不改 mode。
  对应「网络闪断又恢复」后手动重连的场景。

### 跨 NAT 的组合边界（本地 ↔ 云端）

| 组合 | 是否可连 | 原因 |
|---|---|---|
| srflx↔srflx / relay↔relay / srflx↔relay | ✅ | 两端都是公网候选 |
| 含 host 的任意组合 | ❌ | host 是私网（本地 192.168.x / 云端 172.17.14.131），互相够不着 |

> 所以跨 NAT 测「切换流畅性」请用 **srflx / relay** 两档来回切；host 只在同网段（hairpin）有意义。

### 手把手流程

**终端 A（本地 offerer）** 与 **终端 B（云端 answerer）** 分别 `nc -U` 连上后：

```text
# --- 建立通话：answerer 用 auto（host/srflx/relay 都采集，方便任意 offerer mode 配对）
B> set_ice_transports auto
A> set_ice_transports relay
A> call <B的id>
    ... 双方 peer_connection.state=connected

# --- 切模式：offerer relay -> srflx（重建 PC，旧 relay 分配释放）
A> switch_ice srflx
A< {"event":"ice_switched","result":"success"}       # 完成信号
A> get_ice_config                                    # mode=srflx
A> get_stats                                         # selected 类型应为 srflx
A> peer_connection_status                            # connected 仍为 true（流畅）

# --- 切回来：offerer srflx -> relay
A> switch_ice relay
A< {"event":"ice_switched","result":"success"}

# --- answerer 被命令：answerer auto -> relay（offerer 保持 relay 不变）
B> switch_ice relay
B< {"event":"ice_switched","result":"success"}

# --- 重连：ICE 重启（复用旧候选，不重建、不改 mode）
A> reconnect
A< {"event":"reconnected","result":"success"}
A> get_ice_config                                    # mode 仍是 relay（不变）
```

### 要观察的事件

| 事件 | 何时出现 |
|---|---|
| `ice_switched` | `switch_ice` 后（重建 PC 重新连通用 success/failed 报告） |
| `reconnected` | `reconnect` 后（ICE 重启协商完成 success/failed 报告） |
| `signal_client.switch_ice` | 对端收到你的 switch_ice 请求（它会被动重建） |
| `signal_client.reconnect` | 对端收到你的 reconnect 请求（offerer 会被动发起 ICE 重启） |

### 流畅性判据

- 切模式 / 重连期间 `peer_connection.state` 短暂波动（connected→connecting→connected），
  最终稳定在 `connected`，`data_channel.state` 保持 `open`，即算流畅。
- 每次 `switch_ice`/`reconnect` 都应收到对应的 `ice_switched`/`reconnected` success；
  若收到 `failed` 或一直收不到，说明该组合/该网络下连不上（参考上面的组合边界）。
