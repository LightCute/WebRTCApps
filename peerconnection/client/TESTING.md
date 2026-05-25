# WebRTC Daemon Unix Socket 测试指南

通过 `nc` 连接 daemon 的 Unix socket，发送 JSON 命令进行所有操作。

## 启动 daemon

```bash
mkdir -p /tmp/webrtc_test
WEBRTC_RUNTIME_DIR=/tmp/webrtc_test \
  ./out/apps_peerconnection_client/apps_peerconnection_client
# 输出: WebRTC daemon started. Listening on /tmp/webrtc_test/webrtc_ctrl.sock
```

## 连接控制台

```bash
nc -U /tmp/webrtc_test/webrtc_ctrl.sock
```

之后所有命令在此终端中逐行输入，回车发送。**nc 必须保持打开**才能收到异步事件。

---

## 命令参考

通用格式：`{"id":<int>,"cmd":"<命令>"[, "params":{...}]}`

每个命令返回 `{"id":<n>,"ok":true}`（同步确认），部分命令后续还有异步事件推送。

### 1. connect — 连接信令服务器

```
{"id":1,"cmd":"connect","params":{"server":"<IP>","port":<端口>}}
```

异步事件：
- `{"event":"server_connected"}`
- `{"event":"peer_online","peer":{"id":2,"name":"..."}}`
- `{"event":"peer_list","peers":[...]}`

示例：
```
{"id":1,"cmd":"connect","params":{"server":"120.79.210.6","port":8888}}
```

### 2. disconnect — 断开信令服务器

```
{"id":2,"cmd":"disconnect"}
```

异步事件：`{"event":"server_disconnected"}`

### 3. call — 呼叫对端

```
{"id":3,"cmd":"call","params":{"peer_id":<peer_id>}}
```

异步事件：
- `{"event":"call_connected"}`
- `{"event":"ice_state","state":"checking"}` → `"connected"`
- `{"event":"data_channel_state","state":"open"}`

示例：
```
{"id":3,"cmd":"call","params":{"peer_id":2}}
```

### 4. hangup — 挂断

```
{"id":4,"cmd":"hangup"}
```

异步事件：`{"event":"call_disconnected"}` + `{"event":"peer_list",...}`

### 5. send_data — 发送 DataChannel 消息

```
{"id":5,"cmd":"send_data","params":{"text":"<消息内容>"}}
```

对端收到：`{"event":"data_received","text":"<消息内容>"}`

示例：
```
{"id":5,"cmd":"send_data","params":{"text":"Hello from Alice"}}
```

### 6. set_mute — 静音/暂停视频

```
{"id":6,"cmd":"set_mute","params":{"audio":true,"video":false}}
```

`audio` 控制麦克风静音，`video` 控制视频暂停（不发送）。

### 7. query_devices — 查询设备列表

```
{"id":7,"cmd":"query_devices"}
```

异步事件：
- `{"event":"video_devices","devices":[{"idx":0,"name":"..."},...]}`
- `{"event":"audio_input_devices","devices":[{"idx":0,"name":"..."},...]}`

### 8. set_video_device — 切换摄像头

```
{"id":8,"cmd":"set_video_device","params":{"device_idx":0}}
```

`device_idx` 来自 `query_devices` 返回的摄像头列表。

### 9. set_audio_input_device — 切换麦克风

```
{"id":9,"cmd":"set_audio_input_device","params":{"device_idx":0}}
```

`device_idx` 来自 `query_devices` 返回的麦克风列表。

### 10. get_local_sdp — 收集本地 SDP（验证编解码能力）

```
{"id":10,"cmd":"get_local_sdp"}
```

无同步返回。异步事件：
- `{"event":"local_sdp","sdp":"<SDP字符串>"}` — 成功，SDP 中包含完整编解码器列表
- `{"event":"local_sdp_error","error":"..."}` — 失败（如已在通话中）

> **注意**：发送此命令前不要执行 `call`。如果已建立通话，会返回错误。

### 11. shutdown — 关闭 daemon

```
{"id":99,"cmd":"shutdown"}
```

---

## 事件参考

daemon 主动推送的事件一览：

| 事件 | 触发时机 | 关键字段 |
|------|---------|---------|
| `server_connected` | 信令服务器连接成功 | — |
| `server_disconnected` | 与信令服务器断开 | — |
| `server_connection_failed` | 连接信令服务器失败 | `error` |
| `peer_online` | 有对端上线 | `peer.id`, `peer.name` |
| `peer_offline` | 对端下线 | `peer_id` |
| `peer_busy` | 对端正忙（已在通话中） | `peer_id` |
| `peer_list` | 在线列表更新 | `peers[{id,name},...]` |
| `call_connected` | 通话建立 | — |
| `call_disconnected` | 通话结束 | — |
| `ice_state` | ICE 连接状态变化 | `state` (checking/connected/disconnected/failed/closed) |
| `data_channel_state` | DataChannel 状态变化 | `state` (open/closed) |
| `data_received` | 收到 DataChannel 消息 | `text` |
| `video_devices` | query_devices 结果 | `devices[{idx,name},...]` |
| `audio_input_devices` | query_devices 结果 | `devices[{idx,name},...]` |
| `local_sdp` | get_local_sdp 结果 | `sdp` |
| `local_sdp_error` | get_local_sdp 失败 | `error` |

---

## 快速测试流程

### 1. 验证 H264 编解码能力

```
{"id":1,"cmd":"get_local_sdp"}
```

等待 `local_sdp` 事件，检查 SDP video m-line 确认 H264 存在。预期可看到：

```
a=rtpmap:96 H264/90000
a=fmtp:96 profile-level-id=42001f
```

### 2. 双人通话

```
# Alice
{"id":1,"cmd":"connect","params":{"server":"120.79.210.6","port":8888}}
# Bob
{"id":1,"cmd":"connect","params":{"server":"120.79.210.6","port":8888}}
# Alice 呼叫 Bob (peer_id 从 peer_list 获取)
{"id":2,"cmd":"call","params":{"peer_id":2}}
# 发送消息
{"id":3,"cmd":"send_data","params":{"text":"Hello Bob"}}
# 挂断
{"id":4,"cmd":"hangup"}
```

### 3. 验证重呼

挂断后等待 3 秒，再次 `call` 同一对端。预期正常建立通话。

---

## 运行时文件布局

```
/tmp/webrtc_test/                       ← WEBRTC_RUNTIME_DIR
├── webrtc_ctrl.sock                    ← Unix socket
├── shm_video_buf                       ← 视频共享内存
├── shm_audio_cap                       ← 音频采集共享内存
├── shm_audio_playout                   ← 音频播放共享内存
├── daemon.0.log                        ← 日志 (轮转: 5文件 x 10MB)
└── daemon.1.log
```

多实例只需设置不同的 `WEBRTC_RUNTIME_DIR` 实现完全隔离：

```bash
# Alice
WEBRTC_RUNTIME_DIR=/tmp/webrtc_alice ./out/apps_peerconnection_client/apps_peerconnection_client
# Bob
WEBRTC_RUNTIME_DIR=/tmp/webrtc_bob ./out/apps_peerconnection_client/apps_peerconnection_client
```

## 清理

```bash
# 通过 socket 关闭 daemon
echo '{"id":99,"cmd":"shutdown"}' | socat - UNIX-CONNECT:/tmp/webrtc_test/webrtc_ctrl.sock

# 清理临时文件
rm -rf /tmp/webrtc_test
```
