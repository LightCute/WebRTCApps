# 信令服务器测试报告

日期：2026-11-04
状态：全部通过

## 1. 单元测试（3 个，编译后位于 `build/`）

```bash
bash build.sh
./build/messages_test
./build/registry_test
./build/logger_test
```

| 测试 | 覆盖内容 | 结果 |
|---|---|---|
| `messages_test` | 消息编解码：`welcome`/`error`/`roster`/`leave`/`message` 各字段；`StampFrom` 透传保留未知字段（如 `usernameFragment`）；SDP 含引号、换行的转义往返无损；非法/空 JSON 返回 false | 通过 |
| `registry_test` | 状态机：注册分配 id、名字撞车返回 -1；`Call` 的 ok/busy/offline/主叫忙/自呼；`IsPaired`、`GetPeer`；挂断回 idle；`Remove` 返回对端 id 并清对端状态 | 通过 |
| `logger_test` | `MultiLogger` 转发到所有 sink；`FileLogger` 写文件 + 按大小滚动（最新行在、最旧行被滚走） | 通过 |

## 2. 集成测试

用真 `signaling_server` + 真 `test_client`（脚本驱动 stdin）端到端验证，全部通过：

| 场景 | 预期 | 结果 |
|---|---|---|
| 完整通话闭环 | A/B 注册 → roster 广播 → A `call`（含 offer）→ B `answer` → `candidate` 互发 → `leave` 双方收到并回 idle | 通过 |
| 忙线拒呼 | 通话中 C `call` 已忙的 A → C 收到 `error busy` | 通过 |
| 名字撞车 | 新客户端 `register` 已在线名字 → 收到 `error name_taken` | 通过 |
| `get_roster` 查询 | 发起方收到当前 `roster`（不广播） | 通过 |
| 重连 | `reconnect` 后重新 `register` 拿到新 id（旧连接注销） | 通过 |
| 通话中掉线 | 通话中 kill 一端 → 对端收到 `peer_gone`，roster 移除并回 idle | 通过 |
| 重复注册 | 已注册连接再 `register` → 被忽略，不产生新 id | 通过 |
| 文本 relay | `msg` → 服务器转发 → 对端收到 `message{from,text}` | 通过 |

## 3. 专项测试：心跳（ping/pong）

- 场景：裸 socket 完成 WebSocket 握手但**永不回 pong**（模拟半开/卡死连接）。
- 预期：服务器在 `ping_timeout` 后断开该连接。
- 结果：通过（`--ping_interval=1 --ping_timeout=2` 下，约 3 秒后服务器主动发 close frame）。

这条覆盖了 review 阶段发现并修复的 Important 项：**未注册/半开连接之前不被心跳覆盖、会一直泄漏**。修复后 `OnOpen` 即记录连接、心跳遍历全部连接、断开统一清理。

## 4. 已知限制

- 服务器无 TLS/鉴权（明文 `ws://`）。
- 通话级超时未做（呼叫卡住靠手动 `leave` 恢复）。
- 文本消息离线时不缓存，直接回 `error offline`。
