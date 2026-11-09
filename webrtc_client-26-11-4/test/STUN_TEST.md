# STUN 连通性测试 —— 复现指南

日期：2026-11-09
状态：✅ 已通过（2026-11-08 实测）

本文记录如何完整复现一次 **STUN（srflx）连通性测试**：验证两个**不同公网出口**的
WebRTC peer 之间，仅靠 STUN 服务器反射候选（srflx）就能建立 DataChannel 通话，
无需 TURN 中继。

---

## 1. 测试拓扑

```
┌─────────────────────────┐          ┌─────────────────────────┐
│  本机（offerer）          │          │  云服务器（answerer）    │
│  学校 NAT 内网           │          │  阿里云 120.79.210.6    │
│  公网出口 27.39.221.103  │          │  独立公网 IP             │
└───────────┬─────────────┘          └───────────┬─────────────┘
            │  信令走 TCP 8888（WebSocket）        │
            └─────────────── 120.79.210.6:8888 ───┘
            │                                     │
            └── 媒体/DataChannel 走 srflx（UDP，直连打洞）──┘
```

- **信令**：两端先连同一台 `signaling_server`（云服务器 8888 端口），交换 SDP 和 ICE 候选。
- **媒体**：ICE 用谷歌 STUN（`stun:stun.l.google.com:19302`）反射出各自公网地址，
  两端用 srflx 候选直连，**不经信令服务器转发数据**。
- 关键点：两端公网出口不同（本机 `27.39.221.103` vs 云端 `120.79.210.6`），
  所以 **不需要 NAT hairpin**，srflx 能打通。

---

## 2. 前置条件

| 项 | 值 | 说明 |
|---|---|---|
| 云服务器 | `120.79.210.6`，root，密钥登录 | 免密：`/home/light/.ssh/id_ed25519` |
| 信令端口 | `8888`（TCP） | 需在**阿里云安全组放行**（已确认可达） |
| STUN | `stun.l.google.com:19302`（UDP） | 两端都要能出网访问 |
| 本地公网出口 | `27.39.221.103` | 学校 NAT，无 hairpin |

> 快速自检端口可达性（在**本机**执行）：
> ```bash
> timeout 3 bash -c '</dev/tcp/120.79.210.6/8888' && echo "8888 可达"
> ```

---

## 3. 涉及的产物

| 产物 | 路径 | 构建方式 |
|---|---|---|
| `signaling_server` | `signal_server_26_11_3/build/signaling_server` | `bash build.sh` |
| `webrtc_engine_stun_test` | `out/webrtc_client_x64/webrtc_engine_stun_test` | GN/ninja（见下） |
| 测试源码 | `webrtc_client-26-11-4/test/stun_integration_main.cc` | — |

> 仓库根是 `/home/light/webrtc/checkout/src`，下文所有相对路径都相对于它。

---

## 4. 构建（产物已存在可跳过）

### 4.1 信令服务器

```bash
cd /home/light/webrtc/checkout/src/apps/signal_server_26_11_3
bash build.sh          # 产出 build/signaling_server（还有 3 个单测二进制）
```

### 4.2 STUN 测试二进制（GN/ninja，增量）

```bash
cd /home/light/webrtc/checkout/src
ninja -C out/webrtc_client_x64 webrtc_engine_stun_test
```

> 若 `out/webrtc_client_x64` 尚未生成（全新 checkout），先用
> `webrtc_client-26-11-4/build_x64.sh` 做一次 `gn gen` 再 ninja。

验证两个二进制都能跑（glibc 兼容性检查，云端为 glibc 2.39）：

```bash
ldd out/webrtc_client_x64/webrtc_engine_stun_test        # 最高 GLIBC_2.17
ldd signal_server_26_11_3/build/signaling_server          # 最高 GLIBC_2.38
```

---

## 5. 部署到云服务器

在**本机**用 `scp` 上传两个二进制（或走 mcp_ssh 的 `upload_file`）：

```bash
ssh root@120.79.210.6 'mkdir -p /root/stun_test'

scp signal_server_26_11_3/build/signaling_server \
    root@120.79.210.6:/root/stun_test/signaling_server

scp out/webrtc_client_x64/webrtc_engine_stun_test \
    root@120.79.210.6:/root/stun_test/webrtc_engine_stun_test

ssh root@120.79.210.6 'chmod +x /root/stun_test/signaling_server /root/stun_test/webrtc_engine_stun_test'
```

---

## 6. 云服务器：起信令 + answerer

### 6.1（可选）确认 8888 没被占用

```bash
ssh root@120.79.210.6 'ss -tlnp | grep :8888'
```

> ⚠️ 历史上 8888 曾被一个运行 187 天的旧 `peerconnection_server` 占用（无鉴权、公开
> 暴露）。若看到非 `signaling_server` 的进程占用 8888，先停掉它：
> ```bash
> ssh root@120.79.210.6 'kill <pid>'
> ```

### 6.2 启动 signaling_server（后台）

```bash
ssh root@120.79.210.6 'cd /root/stun_test && \
  nohup ./signaling_server --port=8888 --ping_interval=2 \
    > signaling_server.log 2>&1 &'
```

确认监听成功：

```bash
ssh root@120.79.210.6 'sleep 1 && ss -tlnp | grep :8888 && cat /root/stun_test/signaling_server.log'
# 期望看到: listening on port 8888
```

### 6.3 启动 answerer（后台）

```bash
ssh root@120.79.210.6 'cd /root/stun_test && \
  nohup ./webrtc_engine_stun_test \
    --role=answerer --server=127.0.0.1 --port=8888 --ice_transports=srflx \
    > answerer.log 2>&1 &'
```

> `--server=127.0.0.1`：answerer 和信令服务器同在云端，走本机回环。

---

## 7. 本机：起 offerer（前台）

```bash
cd /home/light/webrtc/checkout/src/out/webrtc_client_x64

./webrtc_engine_stun_test \
  --role=offerer --server=120.79.210.6 --port=8888 --ice_transports=srflx
```

- `--server=120.79.210.6`：offerer 从本机跨公网连云端的信令服务器。
- 进程跑完会自动断开退出，整个流程约 10~15 秒。

> 顺序说明：answerer 会等来电最多 30s，offerer 会等 answerer 出现在 roster 最多 30s。
> 所以先起 answerer 再起 offerer（或反过来）都行，只要两者在 ~30s 内先后启动。

---

## 8. 预期结果

### 8.1 offerer（本机 stdout）

```
[offerer] signed in, my_id=<N>
[offerer] calling answerer id=<M>
[offerer] connected=yes selected=prflx
[offerer] local_candidates: host srflx prflx*
[offerer] ok: srflx candidate gathered
```

### 8.2 answerer（云端 `answerer.log`）

```bash
ssh root@120.79.210.6 'cat /root/stun_test/answerer.log | tail -5'
```

```
[answerer] signed in, my_id=<M>
[answerer] incoming call received
[answerer] connected=yes selected=srflx
[answerer] local_candidates: srflx* host
[answerer] ok: srflx candidate gathered
```

### 8.3 判定标准

| 判据 | 说明 |
|---|---|
| 两边都打印 `connected=yes` | 打洞成功，ICE 连通 |
| 两边都打印 `ok: srflx candidate gathered` | 成功采集到 srflx 候选，**offerer 退出码为 0** |
| `selected=...` | 见下节，`srflx` 或 `prflx` 都是**公网反射地址**，均算通过 |

> `my_id` 每次运行会递增（信令服务器的 id 分配器不重置），值不固定。

---

## 9. 结果解读：为什么 offerer 是 `prflx`、answerer 是 `srflx`

- **answerer（云端）**：内网 `172.17.x.x`，经 STUN 反射得到公网 `120.79.210.6:56467`
  （`srflx`）。ICE 确认这个映射后，选中的本端候选就是 `srflx`。
- **offerer（学校 NAT）**：STUN 反射得到 `27.39.221.103:22118`（`srflx`），但学校 NAT
  是**端口相关映射**——往不同目的地发包会用不同的源端口。云端实际看到 offerer 的源地址
  是 `27.39.221.103:2347`，这个"对端学到的反射地址"就是 `prflx`（peer-reflexive）。
  于是 offerer 选中的本端候选是 `prflx`。

两者本质都是**公网反射地址**（不是内网 host 地址），说明「STUN 打洞跨不同公网出口」
成功，这正是本次测试要证明的结论。

### 候选类型速查（详见 `peerconnection/learning/docs/ICE.md`）

| 类型 | 含义 | 优先级 |
|---|---|---|
| `host` | 本机内网地址 | 最高 |
| `srflx` | STUN 服务器反射的公网地址 | 中 |
| `prflx` | 对端反射学到的公网地址 | 中 |
| `relay` | TURN 中继地址 | 最低（兜底） |

本次用 `--ice_transports=srflx`，内部会设 `kNoHost`（屏蔽 host 候选），强制走公网反射
路径，避免两端因内网 host 直连而"假通过"。

---

## 10. 常见问题排查

| 现象 | 原因 | 处理 |
|---|---|---|
| offerer 报 `FAIL: answerer not in roster` | answerer 起太晚/已退出 | 确认 answerer 在跑；两者 ~30s 内先后启动 |
| answerer 报 `FAIL: no srflx candidate gathered` | 谷歌 STUN 不通 | 检查两端能否出网访问 `stun.l.google.com:19302`（UDP） |
| 信令连不上 / 超时 | 8888 未放行或被占用 | 检查阿里云安全组 + `ss -tlnp \| grep :8888` |
| 二进制在云端跑不起来 | glibc 不兼容 | `ldd` 看缺库；云端 glibc 2.39，需 ≥ 编译机所需版本 |
| 日志里 `OnSignalDisconnected` 提前出现 | 对端断开 | 看对端日志找根因（常见是 8888 被抢占） |

---

## 附：CLI 参数表

### `webrtc_engine_stun_test`（源码 `test/stun_integration_main.cc`）

| 参数 | 默认 | 取值 |
|---|---|---|
| `--role=` | `offerer` | `offerer` / `answerer` |
| `--server=` | `127.0.0.1` | 信令服务器地址 |
| `--port=` | `8888` | 信令服务器端口 |
| `--ice_transports=` | `srflx` | `auto` / `host` / `srflx` / `relay` |

> 该测试二进制内部**写死谷歌 STUN**：`stun:stun.l.google.com:19302`。
> 若要测 TURN（`relay`），需先在源码里给 `IceConfig` 补 `turn_uri/turn_user/turn_pass`
> （云服务器 coturn 凭据：`turn:120.79.210.6:3478` / `test` / `123`）。

### `signaling_server`（源码 `signal_server_26_11_3/main.cc`）

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port=` | `8888` | 监听端口 |
| `--ping_interval=` | `10` | 心跳间隔（秒） |
| `--ping_timeout=` | `30` | 心跳超时（秒） |
| `--log_file=` | 空 | 文件日志路径 |
