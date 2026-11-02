# ICE 信息解读指南

ICE（Interactive Connectivity Establishment）负责在 WebRTC 两端之间**找出一条可用的网络路径**（打洞）。本文解析 ICE 的候选信息，说明开发者该关注什么。

## 1. 先看 learning 里的候选格式

learning 通过 IPC 交换候选，格式是 `mline_index|mid|candidate`，例如：

```
0|0|candidate:2680679466 1 udp 2122260223 192.168.1.151 52630 typ host generation 0 ufrag igfe network-id 1 network-cost 50
0|0|candidate:814550292 1 udp 1686052607 27.39.221.103 13489 typ srflx raddr 192.168.1.151 rport 52630 generation 0 ufrag igfe network-id 1 network-cost 50
0|0|candidate:1665156979 1 udp 58662911 120.79.210.6 63026 typ relay raddr 27.39.221.103 rport 13489 generation 0 ufrag igfe network-id 1 network-cost 50
0|0|candidate:3775401650 1 tcp 1518280447 192.168.1.151 49205 typ host tcptype passive generation 0 ufrag igfe network-id 1 network-cost 50
0|0|candidate:814550292 1 udp 1686052607 27.39.221.103 5847 typ srflx raddr 192.168.1.151 rport 52630 generation 0 ufrag igfe network-id 1 network-cost 50
done
```

- 前两段 `0|0|` 是 learning 自己的封装：`mline_index=0`、`mid=0`（对应 SDP 里那条 m-line）。
- 后面 `candidate:...` 才是真正的 ICE 候选（标准 RFC 8445 格式）。

## 2. ICE candidate 的字段（逐字段解读）

标准格式：

```
candidate:<foundation> <component-id> <transport> <priority> <address> <port> typ <type> [raddr <addr>] [rport <port>] [tcptype <tcp-type>] [generation <gen>] [ufrag <ufrag>] [network-id <id>] [network-cost <cost>]
```

以第一个候选为例：

```
candidate:2680679466 1 udp 2122260223 192.168.1.151 52630 typ host generation 0 ufrag igfe network-id 1 network-cost 50
```

| 字段 | 值 | 含义 |
|---|---|---|
| foundation | `2680679466` | 基础标识：标识同一候选集合（同地址/端口的不同传输共享） |
| component-id | `1` | 组件 ID：1 = RTP，2 = RTCP |
| transport | `udp` | 传输协议：udp / tcp |
| priority | `2122260223` | **优先级**：越大越优先尝试 |
| address | `192.168.1.151` | 连接地址（本机可达的 IP） |
| port | `52630` | 端口 |
| typ | `host` | 候选类型（见下节） |
| raddr / rport | — | 相关地址/端口：srflx/relay 的"映射前"地址 |
| tcptype | — | TCP 类型：active / passive / so（仅 TCP） |
| generation | `0` | 代：ICE restart 时递增 |
| ufrag | `igfe` | ICE 用户名片段（须与 SDP 的 `ice-ufrag` 一致） |
| network-id | `1` | 网络接口 ID |
| network-cost | `50` | 网络成本（多网卡选路用，越小越优） |

## 3. 四种候选类型

| 类型 | 说明 | 示例 | 特点 |
|---|---|---|---|
| **host**（主机） | 本机网卡的 IP:port | `192.168.1.151:52630` | 直连最快；跨 NAT 不一定可达 |
| **srflx**（服务器反射） | STUN 服务器反射得到的公网 IP:port | `27.39.221.103:13489`（`raddr` 指回 host） | NAT 穿透的主力 |
| **prflx**（对端反射） | 从对端发来的 STUN 请求源地址学到的候选 | （运行时动态发现，不在此列表） | 见下方说明 |
| **relay**（中继） | TURN 服务器中继的 IP:port | `120.79.210.6:63026` | 兜底，最慢但最可靠 |

说明：

- **srflx 的 `raddr`/`rport`** 指向它"反射前"的 host 地址（`raddr 192.168.1.151 rport 52630`），能看到"内网地址 → 公网地址"的映射。
- **relay 的 `raddr`** 指向 srflx 地址（`raddr 27.39.221.103 rport 13489`），能看到 TURN 是在反射地址之上再中继。
- **prflx** 不在"本地采集"的候选列表里，它是连接过程中动态发现的——之前日志里出现过 `Adding connection from peer reflexive candidate`，就是这个。

## 4. 优先级（priority）与选路

priority 决定 ICE 尝试候选的顺序，值越大越优先。粗略看：

```
priority ≈ (2^24) * type_preference + (2^8) * local_preference + (256 - component_id)
```

type_preference（越大越优先）：

| 类型 | type_preference |
|---|---|
| host | 126 |
| prflx | 110 |
| srflx | 100 |
| relay | 0 |

对照上面的候选：

| 候选 | priority | 顺序 |
|---|---|---|
| host | `2122260223` | ① 最优先 |
| srflx | `1686052607` | ② 次之 |
| relay | `58662911` | ③ 最后 |

所以 ICE 的选路逻辑是：**优先直连 host，打不通再试 STUN 反射 srflx，最后才用 TURN 中继 relay**。

## 5. 开发者关注什么

1. **候选类型是否齐全**：跨 NAT 场景下——
   - 缺 `srflx` → STUN 服务器不通（反射失败）
   - 缺 `relay` → TURN 服务器不通（中继失败）
   - 只有 `host` → 双方在同一内网（直连即可）

2. **地址信息反映 NAT 拓扑**：
   - host `192.168.1.151` → 内网地址
   - srflx `27.39.221.103` → 公网出口地址
   - relay `120.79.210.6` → TURN 服务器地址

3. **优先级**：确认选路顺序（host 优先、relay 兜底）。

4. **ufrag 一致性**：所有候选的 `ufrag` 必须相同（都是 `igfe`），且和 SDP 里的 `ice-ufrag` 匹配，否则 ICE 鉴权失败。

5. **network-cost**：多网卡场景下选更优网卡（越小越好）。

## 6. ICE 的完整信息体系（candidate 之外还有什么）

| 信息 | 在哪 | 作用 |
|---|---|---|
| **candidate（候选）** | 本文件所述 | 本端可达的地址列表 |
| **ice-ufrag / ice-pwd** | SDP 里 | 打洞鉴权凭据（"账号密码"） |
| **连通性检查** | 运行时 STUN binding request/response | 实际"打洞"，探测哪条路通 |
| **candidate pair（候选对）** | ICE agent 内部 | 本地候选 × 对端候选的组合 |
| **selected pair（选中对）** | 日志 `New selected connection` | 最终选中的那条路 |
| **IceConnectionState / PeerConnectionState** | `OnConnectionChange` 等回调 | 连接状态机（checking → connected → completed） |

所以调 ICE 建连时，完整流程是：

```
采集 candidate → 交换 candidate → 配对（candidate pair）→ STUN 连通性检查
   → 选中最优 pair → 连接状态变化（checking → connected）
```

## 7. 一句话总结

ICE 的本质是"**列出我所有可达的地址，和对方所有可达的地址两两配对，挨个试，选最快能通的那条**"。开发者最要盯的是：**候选类型（host/srflx/relay）是否齐全、优先级顺序、ufrag 是否一致**。
