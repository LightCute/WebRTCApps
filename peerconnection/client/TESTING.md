# 本地双客户端通话测试指南

在同一台机器上启动信令服务器 + 两个 WebRTC 守护进程，模拟完整的通话/挂断/重呼流程。

## 前置条件

- 已编译 x64 daemon 二进制（`cd apps/peerconnection/client && ./build.sh`）
- 已编译信令服务器（`cd apps/peerconnection/server && ./build.sh`）

## 架构

```
┌─────────────────┐     ┌─────────────────┐
│  Alice Daemon   │     │   Bob Daemon    │
│  nc → /tmp/     │     │  nc → /tmp/     │
│  alice/ctrl.sock│     │  bob/ctrl.sock  │
└───────┬─────────┘     └───────┬─────────┘
        │ ICE (P2P)             │
        └───────────┬───────────┘
                    │
        ┌───────────┴───────────┐
        │  Signaling Server     │
        │  localhost:8888       │
        └───────────────────────┘
```

## 一、启动信令服务器

```bash
# 终端1：信令服务器
cd apps/peerconnection/server
./build.sh                          # 首次需要编译
./out/signaling_server/peerconnection_server --port=8888

# 输出: Server listening on port 8888
# 日志: tail -f /tmp/webrtc_logs/signaling_server.log
```

## 二、启动 Alice 守护进程

```bash
# 终端2：Alice daemon
mkdir -p /tmp/webrtc_alice
WEBRTC_RUNTIME_DIR=/tmp/webrtc_alice \
  ./out/apps_peerconnection_client/apps_peerconnection_client
# 输出: WebRTC daemon started. Listening on /tmp/webrtc_alice/webrtc_ctrl.sock
```

## 三、启动 Bob 守护进程

```bash
# 终端3：Bob daemon
mkdir -p /tmp/webrtc_bob
WEBRTC_RUNTIME_DIR=/tmp/webrtc_bob \
  ./out/apps_peerconnection_client/apps_peerconnection_client
# 输出: WebRTC daemon started. Listening on /tmp/webrtc_bob/webrtc_ctrl.sock
```

`WEBRTC_RUNTIME_DIR` 保证两个进程的 Unix socket 和 SHM 路径相互隔离。

## 四、打开控制终端

终端4 和终端5 分别连接两个守护进程：

```bash
# 终端4：Alice 控制台
nc -U /tmp/webrtc_alice/webrtc_ctrl.sock
```

```bash
# 终端5：Bob 控制台
nc -U /tmp/webrtc_bob/webrtc_ctrl.sock
```

**重要**：两个 nc 连接必须保持打开。所有后续命令在对应 nc 终端中逐行输入，回车发送。

## 五、第一轮通话

### 5.1 签到

在两个 nc 终端中分别输入：

```
Alice 终端4 >  {"id":1,"cmd":"connect","params":{"server":"127.0.0.1","port":8888}}
Bob   终端5 >  {"id":1,"cmd":"connect","params":{"server":"127.0.0.1","port":8888}}
```
{"id":1,"cmd":"connect","params":{"server":"120.79.210.6","port":8888}}
双方都会收到：
```json
{"id":1,"ok":true}
{"event":"server_connected"}
{"event":"peer_online","peer":{"id":2,"name":"Bob"}}
{"event":"peer_list","peers":[{"id":2,"name":"Bob"}]}
```

> 记下对方的 `peer_id`。Alice 看到 Bob 的 id，Bob 看到 Alice 的 id。

### 5.2 Alice 呼叫 Bob（第一轮）

Alice 终端4：
```
{"id":2,"cmd":"call","params":{"peer_id":2}}
```
(peer_id 改为 Bob 的实际 id)

Alice 侧期望：
```json
{"id":2,"ok":true}
{"event":"call_connected"}
{"event":"ice_state","state":"checking"}
{"event":"ice_state","state":"connected"}        ← 通话建立
{"event":"data_channel_state","state":"open"}    ← DataChannel 就绪
```

Bob 侧同时收到 `call_connected` 和 ICE 事件。

### 5.3 发送 DataChannel 消息

Alice 终端4：
```
{"id":3,"cmd":"send_data","params":{"text":"Hello Bob round 1"}}
```

Bob 终端5 会收到：
```json
{"event":"data_received","text":"Hello Bob round 1"}
```

### 5.4 挂断（第一轮）

Alice 终端4：
```
{"id":4,"cmd":"hangup"}
```

双方都应收到：
```json
{"id":4,"ok":true}
{"event":"call_disconnected"}
{"event":"peer_list","peers":[...]}
```

**验证点 1**：挂断后双方都收到 `call_disconnected` 事件 ✅
**验证点 2**：挂断后 `peer_list` 正常刷新 ✅

## 六、第二轮通话（验证挂断后可重呼）

挂断后等待 3 秒，然后：

### 6.1 Alice 再次呼叫 Bob

Alice 终端4：
```
{"id":5,"cmd":"call","params":{"peer_id":2}}
```

期望：与第一轮相同的 `call_connected` + `ice_state: connected`。

### 6.2 发送消息

Alice 终端4：
```
{"id":6,"cmd":"send_data","params":{"text":"Hello Bob round 2"}}
```

Bob 终端5 收到 `data_received`。

**验证点 3**：第二轮 DataChannel 正常工作 ✅

### 6.3 挂断（第二轮）

Bob 终端5（**这次由 Bob 挂断，测试被动挂断方**）：
```
{"id":5,"cmd":"hangup"}
```

双方收到 `call_disconnected` + `peer_list`。

**验证点 4**：被动方挂断也正常 ✅

## 七、第三轮通话（Bob 主动呼叫 Alice）

### 7.1 Bob 呼叫 Alice

Bob 终端5：
```
{"id":6,"cmd":"call","params":{"peer_id":1}}
```
(peer_id 改为 Alice 的实际 id)

### 7.2 发送消息

Bob 终端5：
```
{"id":7,"cmd":"send_data","params":{"text":"Hello Alice from Bob"}}
```

Alice 终端4 收到 `data_received`。

### 7.3 Alice 挂断

Alice 终端4：
```
{"id":7,"cmd":"hangup"}
```

**验证点 5**：第三轮通话也正常 ✅

## 八、跳过通话轮次的快速测试

如果只想快速验证挂断/重呼逻辑，也可以用 Bob 自环（loopback）调用：

```bash
# Bob 连接自己不需要第二次签到，直接 call 自己即可
# Bob 终端5：
{"id":2,"cmd":"call","params":{"peer_id":<Bob自己的id>}}
```

但 loopback 模式依赖 `--autocall` 和 `loopback_` 变量，不如双进程测试全面。

## 九、清理

```bash
# 终端4、5：Ctrl+C 断开 nc

# 关闭守护进程
echo '{"id":99,"cmd":"shutdown"}' | nc -U -q 0 /tmp/webrtc_alice/webrtc_ctrl.sock
echo '{"id":99,"cmd":"shutdown"}' | nc -U -q 0 /tmp/webrtc_bob/webrtc_ctrl.sock

# 关闭信令服务器
pkill peerconnection_server

# 清理临时文件
rm -rf /tmp/webrtc_alice /tmp/webrtc_bob
```

## 验证清单

| 序号 | 验证项 | 预期信号 | 状态 |
|------|--------|---------|------|
| V1 | 签到 | `server_connected` + `peer_list` | ☐ |
| V2 | Alice 呼 Bob | `call_connected` → `ice_state: connected` → `data_channel_state: open` | ☐ |
| V3 | DataChannel 消息 | Bob 收到 `data_received` | ☐ |
| V4 | 第一轮挂断 | 双方收到 `call_disconnected` + `peer_list` | ☐ |
| V5 | 第二轮呼叫 | `call_connected` → `ice_state: connected` → `data_channel_state: open` | ☐ |
| V6 | 第二轮 DataChannel | 消息正常收发 | ☐ |
| V7 | 第二轮挂断 | 双方收到 `call_disconnected` | ☐ |
| V8 | 第三轮 (Bob 呼 Alice) | 通话建立成功 | ☐ |
| V9 | 第三轮挂断 | 双方收到 `call_disconnected` | ☐ |
| V10 | 守护进程不崩溃 | 测试结束后 daemon 正常响应 `shutdown` | ☐ |

## 运行时文件布局

整个进程的所有产物都在 `$WEBRTC_RUNTIME_DIR`（默认 `/tmp/webrtc_runtime`）下：

```
/tmp/webrtc_alice/                    ← WEBRTC_RUNTIME_DIR=/tmp/webrtc_alice
├── webrtc_ctrl.sock                  ← Unix socket
├── shm_video_buf                     ← 共享内存
├── shm_audio_cap                     ← 共享内存
├── shm_audio_playout                 ← 共享内存
├── daemon.0.log                      ← 日志 (轮转: 5文件 x 10MB)
└── daemon.1.log
```

两个实例只需设置不同的 `WEBRTC_RUNTIME_DIR`，所有文件自动隔离：

| 实例 | WEBRTC_RUNTIME_DIR | socket | 日志 |
|------|-------------------|--------|------|
| Alice | `/tmp/webrtc_alice` | `/tmp/webrtc_alice/webrtc_ctrl.sock` | `/tmp/webrtc_alice/daemon.0.log` |
| Bob | `/tmp/webrtc_bob` | `/tmp/webrtc_bob/webrtc_ctrl.sock` | `/tmp/webrtc_bob/daemon.0.log` |

信令服务器也同样支持 `WEBRTC_RUNTIME_DIR`，默认日志写入该目录下的 `server.log`。

## 调试

如果测试异常，查看日志：

```bash
# daemon 日志（Alice 和 Bob 分开）
tail -f /tmp/webrtc_alice/daemon.0.log
tail -f /tmp/webrtc_bob/daemon.0.log

# 信令服务器日志
tail -f /tmp/webrtc_runtime/server.log

# 实时监控挂断相关事件
tail -f /tmp/webrtc_alice/daemon.0.log | grep -E "HangUp|OnPeer|OnHangingGet|HANGUP|call_disconnected|SendHangUpConfirm|OnClose"

# 三份日志对比（排查挂断流程问题必备）
tail -f /tmp/webrtc_alice/daemon.0.log /tmp/webrtc_bob/daemon.0.log /tmp/webrtc_runtime/server.log
