# WebRTC Engine 测试指南

## 前置条件

1. 信令服务器运行中 (当前: `120.79.210.6:8888`)
2. 至少一个对端客户端在线 (例程 GTK 客户端或另一个 standalone)
3. 已构建: `./build.sh` (x64) 或 `./build_arm64.sh` (ARM64)

## 构建

```bash
cd apps/peerconnection/client
./build.sh         # x64
./build_arm64.sh   # ARM64 交叉编译
```

二进制位置:
- x64: `out/apps_peerconnection_client/apps_peerconnection_client`
- ARM64: `out/arm64_peerconnection_client/apps_peerconnection_client`

---

## 模式一: Standalone CLI 模式

进入交互式命令行，手动输入文本命令控制 WebRTC 通话。

### 启动

```bash
./out/apps_peerconnection_client/apps_peerconnection_client --standalone --server=120.79.210.6 --port=8888 --autoconnect --autocall
```

参数说明:
| 参数 | 作用 |
|------|------|
| `--standalone` | 启用 CLI 交互模式 |
| `--server=IP` | 信令服务器地址 |
| `--port=PORT` | 信令服务器端口 (默认 8888) |
| `--autoconnect` | 启动后自动连接信令服务器 |
| `--autocall` | 有对端上线时自动发起呼叫 |

### 交互命令

启动后出现 `>` 提示符，输入以下命令:

```
connect                             连接信令服务器
disconnect                          断开信令服务器
call <peer_id>                      呼叫指定对端 (如 call 250)
hangup                              挂断当前通话
mute                                静音麦克风
unmute                              取消静音
pause                               暂停视频发送
resume                              恢复视频发送
send <text>                         通过 DataChannel 发送文本
quit / exit                         退出程序
```

### 期望输出

```
=== WebRTC CLI (standalone mode) ===
Commands: connect, disconnect, call <id>, hangup, mute, unmute, pause, resume, send <text>, quit
> Auto-calling peer 250...
[event] {"event":"call_connected"}
> [event] {"event":"ice_state","state":"checking"}
[event] {"event":"ice_state","state":"connected"}
[event] {"event":"data_channel_state","state":"open"}
[event] {"event":"data_received","text":"Test Data from Conductor"}
>
```

看到 `ice_state: connected` 和 `data_received` 即表示通话成功建立。

---

## 模式二: 守护模式 (Unix Socket)

WebRTC 进程以后台守护方式运行，监听 Unix socket 接收 JSON 命令，所有状态/事件通过 socket 以 JSON 推送。

### 启动守护进程

```bash
./out/apps_peerconnection_client/apps_peerconnection_client
# 输出: WebRTC daemon started. Listening on /tmp/webrtc_ctrl.sock
```

### 通信方式

使用 `nc` (netcat) 连接 Unix socket，发送单行 JSON 命令，读取 JSON 响应和事件。

```bash
echo '<JSON命令>' | nc -U /tmp/webrtc_ctrl.sock
```

### 完整测试流程

#### 1. 连接信令服务器

```bash
echo '{"id":1,"cmd":"connect","params":{"server":"120.79.210.6","port":8888}}' | nc -U /tmp/webrtc_ctrl.sock
```

期望响应和事件:
```json
{"id":1,"ok":true}
{"event":"server_connected"}
{"event":"peer_online","peer":{"id":250,"name":"cat@lubancat"}}
{"event":"peer_list","peers":[{"id":250,"name":"cat@lubancat"}]}
```

#### 2. 呼叫对端

拿到上一步中的 `peer_id`，发起呼叫:

```bash
echo '{"id":2,"cmd":"call","params":{"peer_id":250}}' | nc -U /tmp/webrtc_ctrl.sock
```

期望响应和事件:
```json
{"id":2,"ok":true}
{"event":"call_connected"}
{"event":"ice_state","state":"checking"}
{"event":"ice_state","state":"connected"}
{"event":"ice_state","state":"completed"}
{"event":"data_channel_state","state":"open"}
{"event":"data_received","text":"Test Data from Conductor"}
```

#### 3. 发送 DataChannel 消息

```bash
echo '{"id":3,"cmd":"send_data","params":{"text":"Hello from daemon"}}' | nc -U /tmp/webrtc_ctrl.sock
```

期望: `{"id":3,"ok":true}`

#### 4. 媒体控制

```bash
echo '{"id":10,"cmd":"set_mute","params":{"audio":true,"video":false}}' | nc -U /tmp/webrtc_ctrl.sock
```

#### 5. 挂断

```bash
echo '{"id":4,"cmd":"hangup"}' | nc -U /tmp/webrtc_ctrl.sock
```

期望:
```json
{"id":4,"ok":true}
{"event":"call_disconnected"}
```

#### 6. 断开信令

```bash
echo '{"id":5,"cmd":"disconnect"}' | nc -U /tmp/webrtc_ctrl.sock
```

期望:
```json
{"id":5,"ok":true}
{"event":"server_disconnected"}
```

#### 7. 关闭守护进程

```bash
echo '{"id":99,"cmd":"shutdown"}' | nc -U /tmp/webrtc_ctrl.sock
```

---

## JSON 协议参考

### 命令 (发送)

| cmd | params | 说明 |
|-----|--------|------|
| `connect` | `server`, `port` | 连接信令服务器 |
| `disconnect` | - | 断开信令服务器 |
| `call` | `peer_id` | 呼叫对端 |
| `hangup` | - | 挂断通话 |
| `shutdown` | - | 关闭守护进程 |
| `set_mute` | `audio`, `video` | 设置静音 (bool) |
| `set_video_pause` | `pause` | 暂停/恢复视频 (bool) |
| `send_data` | `text` | DataChannel 发送文本 |

每条命令带 `id` 字段用于响应对应。

### 响应

```json
{"id":<命令id>, "ok":true}
{"id":<命令id>, "ok":false, "error":"<错误描述>"}
```

### 事件 (无 id)

| event | 附加字段 | 说明 |
|-------|---------|------|
| `server_connected` | - | 信令连接成功 |
| `server_disconnected` | - | 信令断开 |
| `server_connection_failed` | `error` | 信令连接失败 |
| `peer_online` | `peer` | 对端上线 |
| `peer_offline` | `peer_id` | 对端下线 |
| `peer_list` | `peers` | 在线对端列表 |
| `call_connected` | - | 通话建立 |
| `call_disconnected` | - | 通话断开 |
| `ice_state` | `state` | ICE 状态: checking/connected/completed/failed/disconnected |
| `data_channel_state` | `state` | DataChannel 状态: connecting/open/closing/closed |
| `data_received` | `text` | 收到 DataChannel 文本 |

---

## 关键验证项

| 验证项 | Standalone 信号 | Daemon 信号 |
|--------|---------------|------------|
| 服务器连接 | 无明显报错 | `server_connected` 事件 |
| Peer 发现 | `peer_online` 事件 + 自动呼叫 | `peer_online` 事件 |
| 通话建立 | `call_connected` 事件 | `call_connected` 事件 |
| ICE 连接 | `ice_state: connected` | `ice_state: connected` |
| 视频传输 | 无崩溃，RTP 日志 | 守护进程无崩溃 |
| DataChannel | `data_received` 事件 | `data_received` 事件 |
| 挂断 | `call_disconnected` 事件 | `call_disconnected` 事件 |

---

## 调试

查看 WebRTC 内部日志 (stderr):

```bash
# 守护模式 - 查看日志
./out/apps_peerconnection_client/apps_peerconnection_client 2>&1 | tee daemon.log

# Standalone 模式 - 查看日志
./out/apps_peerconnection_client/apps_peerconnection_client --standalone --server=120.79.210.6 2>&1 | tee cli.log
```
