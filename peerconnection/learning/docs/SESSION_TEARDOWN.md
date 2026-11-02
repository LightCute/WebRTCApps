# 会话结束（挂断）流程 —— learning 实现

本文总结 `learning` 工程里「一轮通话结束」时，`learning_common.cc` 具体做了什么。理论依据见同目录 `HANGUP_TEARDOWN.md`。

核心对象生命周期（多轮通话）：

| 对象 | 生命周期 |
|---|---|
| `PeerConnectionFactory`（`g_factory`） | **创建一次，全程复用，不销毁** |
| `PeerConnection`（`g_pc`） | **每轮重建**（`Close()` 不可逆） |
| `DataChannel`（`g_dc`） | **每轮重建**（随 PC `Close()` 自动关闭） |

## 1. 会话结束的触发路径（谁会调 `CleanupCall`）

| 触发 | 来源 | 是否延迟 |
|---|---|---|
| 本地 `/quit`（聊天中） | `HandleLine` → `Hangup()` → 发 `/bye` → `CleanupCall()` | 否（是 posted 任务） |
| 收到对端 `/bye` | `OnMessage` → `PostTask(CleanupCall)` | **是**（DataChannel 回调里不能同步 Close） |
| DataChannel 关闭 | `OnStateChange(kClosed/kClosing)` → 若 `g_in_call` → `PostTask(CleanupCall)` | **是** |
| 客户端断开 | `OnClientDisconnected` → `g_client_id = -1` → `CleanupCall()` | 否（是 posted 任务） |
| 创建 DataChannel 失败 | `StartOffer` 错误路径 → `CleanupCall()` | 否 |

## 2. `CleanupCall()` 做什么（逐步）

```cpp
void CleanupCall() {
    if (!g_in_call) return;      // ① 幂等：没有进行中的通话就直接返回
    g_in_call = false;           // ② 先置位，防止 Close 触发的 OnStateChange 重入

    if (g_pc) { g_pc->Close(); g_pc = nullptr; }  // ③ Close 会自动关所有 DataChannel
    g_dc = nullptr;                                //    释放引用即可，不用手动 Close

    // ④ 重置本轮会话状态（注意：不重置 g_client_id，客户端仍连着）
    g_phase = SignalPhase::kAwaitSdp;
    g_peer_sdp_lines.clear();
    g_candidates_sent = false;
    g_local_candidates.clear();
    g_candidate_count = 0;
    g_local_sdp.clear();

    RTC_LOG(LS_INFO) << "Call cleaned up, waiting for next client...";

    // ⑤ 自动开启下一轮：客户端还在就立即新建 PC（offerer 顺带发 offer）
    if (g_client_id != -1) {
        StartCall();
    }
}
```

要点：

- **① 幂等**：`g_in_call` 既是「是否在通话」标志，也是防重入锁。
- **③ `Close()` 一网打尽**：不需要逐个 `g_dc->Close()`，PC 关闭时 SCTP transport 会 close 所有 DataChannel。
- **④ 保留 `g_client_id`**：客户端（socat/前端进程）的连接跨轮保持，只有客户端真正断开才清掉它。
- **⑤ 自动重开**：通话结束不退出程序，直接进入下一轮。

## 3. `StartCall()` —— 开启一轮（含自动重开）

```cpp
void StartCall() {
    // 复用 g_factory + g_config 新建 PC
    auto err_or = g_factory->CreatePeerConnectionOrError(g_config, std::move(pc_deps));
    if (!err_or.ok()) { RTC_LOG(LS_ERROR) << ...; return; }  // 失败则本轮不开启

    g_pc = std::move(err_or.value());
    g_in_call = true;

    if (g_role == Role::kOfferer) {
        StartOffer();   // 创建 DataChannel + CreateOffer → 推新 SDP 给前端
    }
    // answerer 不主动发，等对端 offer 到达
}
```

所以多轮通话的完整循环是：

```
客户端连入 → StartCall（新建 PC，offerer 发 offer）
   → SDP/ICE 握手 → DataChannel open → 聊天
   → /quit 或 /bye 或断开 → CleanupCall（Close + 重置 + 自动 StartCall）
   → 下一轮 …
（直到客户端断开，g_client_id = -1 才停止自动重开）
```

## 4. 关键设计原则

1. **factory 复用、PC 每轮重建**：`g_factory` 只在 `Shutdown()`（退出程序）里销毁；`g_pc`/`g_dc` 每轮 `Close` 后新建。
2. **不要在 DataChannel 回调里同步 `Close()`**：`OnMessage`/`OnStateChange` 里必须 `PostTask` 延迟到回调返回后再 `CleanupCall`，否则回调栈上的对象被销毁 → use-after-free（这是之前 offer 端 core dump 的根因）。
3. **`g_in_call` 区分「主动关闭」和「自发关闭」**：我们自己 `Close()` 时已把 `g_in_call` 置 false，此时 `OnStateChange(kClosed)` 不会再触发一次多余的清理；只有对端突然关掉 DataChannel（`g_in_call` 仍为 true）才走清理。
4. **`g_client_id` 跨轮保留**：它是「客户端连接」的标识，不是「本轮会话」的标识，所以 `CleanupCall` 里不清它，只在 `OnClientDisconnected` 里清。

## 5. 一次挂断的完整时序（answerer 输入 `/quit`）

```
answerer                                offerer
   │                                      │
 HandleLine("/quit")                      │
   → Hangup() 发 /bye                     │
   → CleanupCall()                        │
        Close PC + 重置状态                │
        StartCall() 新建 PC（等 offer）     │
   │                                      │
   │        （/bye 走 DataChannel）────────→ OnMessage("/bye")
   │                                      │    → PostTask(CleanupCall)
   │                                      │
   │                               CleanupCall()
   │                                      │    Close PC + 重置状态
   │                                      │    StartCall() 新建 PC + CreateOffer
   │                                      │    → 推新 offer SDP 给前端
   │        （新一轮 offer 由人/中继转发）←──┘
   │                                      │
   两端进入第二轮 …
```

## 6. 与 `HANGUP_TEARDOWN.md` 的对应

| 理论结论 | 本实现落点 |
|---|---|
| `Close()` 不可逆，PC 必须重建 | 每轮 `StartCall()` 里 `CreatePeerConnectionOrError` |
| DataChannel 随 `Close()` 自动关 | `CleanupCall()` 只 `Close()` PC，不逐个关 DC |
| factory 可复用 | `g_factory` 只在 `Shutdown()` 销毁 |
| 先停媒体管线 → Close → 释放引用 | 本工程无媒体管线（纯 DataChannel），故省略第一步 |
