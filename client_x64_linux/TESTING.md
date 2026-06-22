# WebRTC client_x64_linux 测试指南

通过 `nc` 连接 daemon 的 Unix socket，发送 JSON 命令进行操作和统计采集。

## 启动 daemon

```bash
mkdir -p /tmp/webrtc_runtime
./out/client_x64_linux/client_x64
# 输出: WebRTC x64 daemon started. Listening on /tmp/webrtc_runtime/webrtc_ctrl.sock
```

## 连接控制台

```bash
nc -U /tmp/webrtc_runtime/webrtc_ctrl.sock
```

所有命令逐行输入，回车发送。**nc 必须保持打开**才能收到异步事件和 stats 推送。

---

## 命令参考

格式：`{"id":<int>,"cmd":"<命令>"[, "params":{...}]}`

返回：`{"id":<n>,"ok":true}` 或 `{"id":<n>,"ok":false,"error":"..."}`

### connect — 连接信令服务器

```
{"id":1,"cmd":"connect","params":{"server":"<IP>","port":<端口>}}
```

异步事件：
- `{"event":"server_connected"}`
- `{"event":"peer_online","peer":{"id":2,"name":"..."}}`
- `{"event":"peer_list","peers":[...]}`

### disconnect — 断开信令服务器

```
{"id":2,"cmd":"disconnect"}
```

事件：`{"event":"server_disconnected"}`

### call — 呼叫对端

```
{"id":3,"cmd":"call","params":{"peer_id":<peer_id>}}
```

事件：
- `{"event":"call_connected"}`
- `{"event":"ice_state","state":"checking"}` → `"connected"`
- `{"event":"stats"}` — 开始收到统计数据

### hangup — 挂断

```
{"id":4,"cmd":"hangup"}
```

事件：`{"event":"call_disconnected"}`

### send_data — 发送 DataChannel 消息

```
{"id":5,"cmd":"send_data","params":{"text":"<消息>"}}
```

### get_local_sdp — 收集本地 SDP

```
{"id":10,"cmd":"get_local_sdp"}
```

事件：`{"event":"local_sdp","sdp":"<SDP>"}` 或 `{"event":"local_sdp_error","error":"..."}`

### dump_stats — 获取实时统计数据

```
{"id":6,"cmd":"dump_stats"}
```

无同步返回。事件 `{"event":"stats",...}` 包含：

| 字段 | 含义 | 正常范围 |
|------|------|---------|
| `encode_fps` | 编码帧率 | 25-30 |
| `encode_w` / `encode_h` | 编码分辨率 | 640x480 (默认) |
| `frames_enc` | 累计编码帧数 | 递增 |
| `quality_limit` | 降质原因 | `"none"` / `"cpu"` / `"bandwidth"` |
| `nack_sent` | 发送端 NACK 数 | 0 (无丢包时) |
| `pli_sent` | 发送端 PLI 数 | 0 |
| `target_kbps` | 目标编码码率 | 300-2000 |
| `decode_fps` | 解码帧率 | 25-30 |
| `pkt_lost` | 丢包数 | 0 |
| `jitter_s` | 抖动 (秒) | <0.005 |
| `rtt_s` | 往返延迟 (秒) | <0.05 |
| `avail_kbps` | 可用带宽 (kbps) | 500-8000 |

### shutdown — 关闭 daemon

```
{"id":99,"cmd":"shutdown"}
```

---

## 事件参考

| 事件 | 触发时机 | 关键字段 |
|------|---------|---------|
| `server_connected` | 信令服务器连接成功 | — |
| `server_disconnected` | 信令服务器断开 | — |
| `server_connection_failed` | 连接信令服务器失败 | `error` |
| `peer_online` | 对端上线 | `peer.id`, `peer.name` |
| `peer_offline` | 对端下线 | `peer_id` |
| `peer_list` | 在线列表更新 | `peers[{id,name},...]` |
| `call_connected` | 通话建立 | — |
| `call_disconnected` | 通话结束 | — |
| `ice_state` | ICE 状态变化 | `state` |
| `data_channel_state` | DC 状态变化 | `state` |
| `data_received` | 收到 DC 消息 | `text` |
| `local_sdp` | get_local_sdp 结果 | `sdp` |
| `local_sdp_error` | get_local_sdp 失败 | `error` |
| `stats` | dump_stats 结果 | 见上表 |

---

## 测试场景

### 正常通话 baseline

```bash
# Alice
{"id":1,"cmd":"connect","params":{"server":"120.79.210.6","port":8888}}
{"id":2,"cmd":"call","params":{"peer_id":2}}
{"id":3,"cmd":"dump_stats"}
# 预期: quality_limit="none", pkt_lost=0, encode_fps≈30, rtt_s<0.05
```

### 限制带宽

```bash
sudo tc qdisc add dev lo root netem rate 500kbit
# dump_stats → quality_limit="bandwidth", avail_kbps≈500, target_kbps 下降
# 恢复: sudo tc qdisc del dev lo root
```

### 模拟丢包

```bash
sudo tc qdisc add dev lo root netem loss 5%
# dump_stats → pkt_lost>0, nack_sent 增加, pli_sent 增加
# 恢复: sudo tc qdisc del dev lo root
```

### 模拟延迟

```bash
sudo tc qdisc add dev lo root netem delay 100ms
# dump_stats → rtt_s≈0.1
# 恢复: sudo tc qdisc del dev lo root
```

---

## 运行时文件

```
/tmp/webrtc_runtime/
├── webrtc_ctrl.sock    ← Unix socket
├── shm_video_buf       ← 视频共享内存 (SHM)
├── daemon.0.log        ← 日志 (5文件 x 10MB)
└── daemon.1.log
```
