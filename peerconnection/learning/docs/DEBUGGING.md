# learning 调试指南（日志 checklist）

本文说明调试 `learning`（WebRTC DataChannel 建连）时，看日志该重点盯什么。配合 `ARCHITECTURE.md`（通信流程）和 `E2E_TEST.md`（测试方法）使用。

日志文件在 socket 同目录下，形如 `learning_offer_<年月日_时分秒>.log` 和 `learning_answer_<...>.log`。

## 1. 一次成功通话的关键里程碑（速查表）

以 offerer 为例，answerer 镜像（差异见表格下方说明）：

| 阶段 | 关键日志（offerer） | 含义 |
|---|---|---|
| 启动 | `started as OFFERER` / `CreatePeerConnection init success!` | 工厂 + PC 就绪 |
| 证书 | `Making key pair` / `Making certificate` | 生成 DTLS 自签证书（DataChannel 加密用） |
| 生成 offer | `Local SDP created (type= offer)` / `SignalingState: have-local-offer` / `SetLocalDesc OK` | offer 已生成并设为本地 |
| ICE 采集 | `IceGatheringState: gathering` → 若干 `ICE candidate` → `IceGatheringState: complete` | host/srflx/relay 候选采集完毕 |
| 收 answer | `Received remote ICE parameters` / `SetRemoteDescription OK` / `SignalingState: stable` | answer 已应用 |
| 发候选 | `Sent N local candidates` | 本地候选发给对端 |
| ICE 检查 | `IceConnectionState new => checking` / `PeerConnectionState: connecting` / `Sent STUN BINDING request` / `Received STUN BINDING response` / `New selected connection` | 打洞、选出最佳路径 |
| DTLS | `Start DTLS handshake` / `DTLS handshake complete` | 加密握手完成 |
| SRTP | `SRTP activated` | 数据加密参数就绪 |
| SCTP | `DcSctpTransport Connecting` / `OnConnected` | DataChannel 底层传输就绪 |
| 连接 | `PeerConnectionState: connected` / `>>> CONNECTION ESTABLISHED <<<` | **传输层连通** |
| 通道 | `DataChannel state: open` | **应用层可收发** |
| 聊天 | `DataChannel onMessage: xxx` | 实际数据流通 |

answerer 的差异点只有两处：`SignalingState: have-remote-offer`（收 offer）和 `Local SDP created (type= answer)`（生成 answer），其余完全对称。

## 2. 调试 checklist（重点盯什么）

1. **三个状态机的转换是否完整**
   - `SignalingState`：`stable → have-local-offer / have-remote-offer → stable`
   - `IceGatheringState`：`gathering → complete`
   - `PeerConnectionState`：`connecting → connected`（这条最关键）

2. **`SetLocalDesc OK` + `SetRemoteDescription OK` 都出现**
   - 说明 SDP 在两端都成功 apply。缺一个就是 SDP 解析或设置失败。

3. **`IceGatheringState: complete`**
   - 候选是否采齐。卡在 `gathering` 通常是 STUN/TURN 探测不通或超时。

4. **候选类型是否齐全：`host` / `srflx` / `relay`**
   - 跨 NAT 场景若缺 `srflx`（STUN 不通）或缺 `relay`（TURN 不通），就是打洞失败的方向。

5. **`New selected connection`**
   - 看最终选了哪条 candidate pair（host/srflx/relay），直接告诉你实际走的路。

6. **`STUN BINDING request/response` 的 rtt**
   - 连通性检查是否真的通了。本机/局域网通常几百 us，跨网几十 ms。

7. **`DTLS handshake complete` + `SRTP activated` + `DcSctpTransport OnConnected`**
   - 三层（DTLS 加密、SRTP 参数、SCTP 传输）都就绪。

8. **`DataChannel state: open`**
   - 最终"能聊天"的标志。注意它晚于 `CONNECTION ESTABLISHED`。

9. **告警类，分情况**
   - TURN `401`：首次鉴权挑战，正常，会自动重试。
   - TURN `403`：权限拒绝，本例无害（连接被 pruned）。
   - `Assuming benign blocking error`：socket 非阻塞读的空转，无害。
   - `-- error when we want to read`：TLS 握手期间的正常读等待，无害。

## 3. 两个特别值得注意的点

### 3.1 answerer 用 prflx 候选连上（正常）

answerer 日志可能出现：

```
Received STUN BINDING request ... from unknown address <IP:port>
Adding connection from peer reflexive candidate: ... prflx ... <IP:port>
```

answerer 在**还没显式收到 offerer 的候选**时，就从 offerer 发来的 STUN 请求源地址**反推出 prflx 候选**并建连。这是 ICE 的 peer-reflexive 机制，正常行为。

### 3.2 `Local ICE candidate [0|]: ` 是误导性日志（已知 bug）

若看到：

```
Local ICE candidate [0|]:     ← mid 和候选串都是空的
```

这是**假象**——候选数据其实是好的。原因是 `OnIceCandidate` 里 `std::move(info)` 在 `RTC_LOG` 之前执行，日志读的是"搬空"后的 `info`。不影响功能，但会误导调试。
