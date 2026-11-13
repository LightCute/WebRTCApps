# webrtc_engine 客户端测试报告

日期：2026-11-05（第 1–13 节）；2026-11-13 追加第 14–27 节（ICE 切换 / 视频全链路 / 抗压 / 断网）
状态：全部通过

模块：`webrtc_engine`（经典三线程 WebRTC 客户端：signaling / worker / network），
上层通过 unix_socket（`ipc_test26_10_26` 的 `UnixSocketServer` + `EngineController`）交互。
命令方向为纯文本，事件方向为 JSON（每行一个对象）。

构建：`bash build_x64.sh` → 产物在 `out/webrtc_client_x64/`。

## 1. 冒烟测试 `webrtc_engine_smoke`

```bash
out/webrtc_client_x64/webrtc_engine_smoke
```

| 覆盖内容 | 结果 |
|---|---|
| 三线程启动 + `PeerConnectionFactory` + `PeerConnection` + `DataChannel` 创建，断言各「已创建」标志 | 通过 |
| 反复创建/销毁 20 次（含工厂/PC/DC），验证析构顺序安全、无泄漏崩溃 | 通过 |

输出：`webrtc_engine_smoke: all pass`。

## 2. 信令集成测试 `webrtc_engine_signaling_test`

```bash
bash test/integration_test.sh   # 起真信令服务器(9799) + 本测试二进制
```

| 覆盖内容 | 结果 |
|---|---|
| 连真信令服务器并 `signed_in`，`my_id` 有效 | 通过 |
| 回调确实运行在 signaling 线程（`callback_on_signaling_thread`） | 通过 |

输出：`signaling integration ok: my_id=1`。

## 3. 通话集成测试 `webrtc_engine_call_test`

```bash
bash test/call_integration_test.sh   # 起真信令服务器(9899) + 本测试二进制
```

进程内起 offerer / answerer 两个引擎，走完整 offer/answer 闭环：

| 覆盖内容 | 结果 |
|---|---|
| 双方注册 → `call` → offer/answer + candidate → PC `connected` | 通过 |
| `DataChannel` 双端 `open` | 通过 |
| `SendDataChannelMessage("hello")` → 对端 `last_received_message() == "hello"` | 通过 |

输出：`call integration ok: connected, DataChannel open, message delivered`。

## 4. 挂断 / 掉线清理测试 `webrtc_engine_teardown_test`

```bash
bash test/teardown_integration_test.sh   # 起真信令服务器(9898) + 本测试二进制
```

| 场景 | 覆盖内容 | 结果 |
|---|---|---|
| 通话中 A 主动 `leave` | 双方 `is_connected()==false` 且 PC 已清理（`is_peer_connection_created()==false`） | 通过 |
| 通话中 A 掉线（断信令） | B 收到 `peer_gone`，`is_connected()==false` 且 PC 已清理 | 通过 |

输出：`leave cleanup ok` / `peer_gone cleanup ok` / `teardown integration ok`。

## 5. 多轮通话测试 `webrtc_engine_multicall_test`

```bash
bash test/multicall_integration_test.sh   # 起真信令服务器(9897) + 本测试二进制
```

登录一次，重复 20 次：`call` → PC connected → DC open → 发消息（每轮消息不同）→ `leave` → 双方清理。

| 覆盖内容 | 结果 |
|---|---|
| 20 轮通话/挂断，无状态残留、无崩溃 | 通过 |

输出：`multicall integration ok: 20 rounds`。

## 6. IPC 集成测试 `ipc_integration_test.py`

```bash
python3 test/ipc_integration_test.py
```

起真信令服务器(9896) + 两个 `webrtc_engine_main` 进程，各连各的 unix_socket，
用文本命令驱动、按 JSON 事件断言：

| 覆盖内容 | 结果 |
|---|---|
| 连接成功收到握手 `{"event":"ipc.connected","commands":[...]}`，命令清单含全部 10 条 | 通过 |
| `register` → `signed_in`（id=1/2） | 通过 |
| `get_roster` → roster 带两人且含 `state`（idle） | 通过 |
| `peer_connection_status` → `signed_in=true` | 通过 |
| `call 2` → 双方 `peer_connection.state=connected` + 对端 `data_channel.state=open` | 通过 |
| `data_channel_msg hello-from-ipc-test` → 对端 `data_channel_msg.text` 一致 | 通过 |
| `get_stats` → `stats` 快照（data_channel 状态/计数、transport、candidate_pair RTT） | 通过 |
| `leave` → 双方 `peer_connection.state=closed` | 通过 |
| `quit` → 两个引擎进程正常退出 | 通过 |

输出：`ipc integration ok: register/call/msg/leave/quit via unix_socket (json)`。

## 7. IPC 多轮通话测试 `ipc_multicall_test.py`

```bash
python3 test/ipc_multicall_test.py
```

起真信令服务器(9895) + 两个 `webrtc_engine_main`，unix_socket 登录一次后，
重复 20 次：`call` → connected → DC open → `data_channel_msg`（每轮不同）→ `leave` → closed。

| 覆盖内容 | 结果 |
|---|---|
| 登录一次，20 轮通话/挂断走 IPC，无残留、无崩溃 | 通过 |

输出：`ipc multicall ok: 20 rounds (login once)`。

## 8. IPC 信令控制测试 `ipc_signaling_test.py`

```bash
python3 test/ipc_signaling_test.py
```

起两个信令服务器(9893/9892) + 两个 `webrtc_engine_main`，覆盖无需通话的信令控制与转发：

| 覆盖内容 | 结果 |
|---|---|
| `register <name> <ip> <port>` 显式指定服务器 | 通过 |
| `peer_connection_status`（空闲）→ `signed_in=true`、`connected=false` | 通过 |
| `signal_msg <peer_id> <text>` 经信令服务器转发（无通话） | 通过 |
| `signal_msg <离线id>` → `signal_client.error` code=offline | 通过 |
| `register` 切换服务器 | 通过 |
| `signal_disconnect` → `disconnected` 且状态清理 | 通过 |
| 断开后 `register` 重连 | 通过 |

输出：`ipc signaling ok: register(ip/port)/switch/disconnect/reconnect/status/signal_msg`。

## 9. IPC 断开清理测试 `ipc_disconnect_test.py`

```bash
python3 test/ipc_disconnect_test.py
```

起真信令服务器(9891) + 两个 `webrtc_engine_main`，覆盖通话中的信令断开清理：

| 覆盖内容 | 结果 |
|---|---|
| 通话中 `peer_connection_status` → `connected=true`、`data_channel_open=true` | 通过 |
| 通话中 `signal_disconnect` → 本端 disconnected + 双方 `peer_connection.state=closed` | 通过 |
| 通话中 kill 信令服务器 → 双方 disconnected + `peer_connection.state=closed` | 通过 |

输出：`ipc disconnect ok: signal_disconnect + server crash teardown`。

## 10. 受理校验 + engine.error 单测 `webrtc_engine_validation_test`

```bash
out/webrtc_client_x64/webrtc_engine_validation_test
```

| 覆盖内容 | 结果 |
|---|---|
| bool 语义：未建 DC `SendDataChannelMessage` 返回 false；未连服务器 `SendSignalMessage` 返回 false | 通过 |
| engine.error：无 factory 建 PC → `peer_connection_failed`；无 PC 建 DC → `data_channel_failed`；非法远端 offer → `set_remote_desc_failed` | 通过 |

输出：`validation ok: bool semantics + engine.error`。

## 11. 命令失败返回 IPC 测试 `ipc_failure_test.py`

```bash
python3 test/ipc_failure_test.py
```

起真信令服务器(9890) + 三个 `webrtc_engine_main`（第三个指向无服务监听的 9889），断言各命令的失败返回：

| 覆盖内容 | 结果 |
|---|---|
| 连接不可达 `register` → `signal_client.error` code=`connect` | 通过 |
| 未通话 `data_channel_msg` → `data_channel_not_open` | 通过 |
| 未注册 `signal_msg` → `signal_send_failed` | 通过 |
| 未注册 `get_roster`/`leave`/`call`/`signal_disconnect` → `not_signed_in` | 通过 |
| 已登录但不在通话 `leave` → `not_in_call` | 通过 |
| 通话中再 `call` → `already_in_call` | 通过 |

输出：`ipc failure ok: connect/data_channel_not_open/signal_send_failed/not_signed_in/not_in_call/already_in_call`。

## 12. STUN 候选收集测试 `webrtc_engine_stun_test`

```bash
bash test/stun_integration_test.sh   # 本机 answerer+offerer 双角色
```

测试拆成两个角色（`--role=answerer|offerer`），各连同一信号服务器，配谷歌 STUN + `srflx` 模式（屏蔽 host），验证 srflx 候选收集：

| 覆盖内容 | 结果 |
|---|---|
| `--role=answerer`：连信号服务器 → 等来电 → 自动应答 → 收集候选 | 通过 |
| `--role=offerer`：连信号服务器 → 从 roster 找 answerer → call → 收集候选 | 通过 |
| 双方 `get_stats` 的 `local_candidates` 含 `srflx`（谷歌 STUN 可达） | 通过 |
| srflx 端到端连通 | 环境相关（同 LAN 同 NAT 需 hairpin；需不同网络才能真连通） |

输出：`[role] ok: srflx candidate gathered`。

> 跨机跑：把 `webrtc_engine_stun_test` 拷到远端（可用 `apps/mcp_ssh` 的 MCP 工具），远端跑 `--role=answerer --server=<本机LAN IP>`，本机跑 `--role=offerer`。

## 13. 跨 NAT 断开/重连抗压测试 `reconnect_integration_test.py`

```bash
python3 test/reconnect_integration_test.py                  # 全量：2 方向 × 8 项 × 5 遍
python3 test/reconnect_integration_test.py --direction A --items offer_leave --cycles 1  # 冒烟
```

驱动真实 `webrtc_engine_main`（本地 + 云端各一个，走 unix socket IPC），反复「建立通话 → 断开 → 重连」。覆盖 **8 种断开方式 × 2 个角色方向 × 每项 5 遍**：

| 断开方式 | 重连机制 |
|---|---|
| `leave` | 同进程再 call |
| `signal_disconnect`（软断信令） | 同进程重连信令再 call |
| `quit`（进程优雅退出） | 重启进程 |
| `SIGKILL`（强杀） | 重启进程 |

| 覆盖内容 | 结果 |
|---|---|
| 16 项（2 方向 × 8 项）全部通过 | 通过 |
| 每项 1 初始建立 + 5 次断连 = 6 次 connected | 通过 |
| 断开/失败后 busy 状态正确清理、可再连 | 通过 |

> 前置：云端信令服务器 8888 + 云端 daemon `.env`（`WEBRTC_MIN_PORT/MAX_PORT=50000/50019`）+ 阿里云安全组放行 8888(TCP)、50000–50019(UDP)。
> 固定端口是为让 srflx 跨 NAT 打洞（云端端口保留型 NAT 把本机固定端口原样映射到公网）。手动复现见 [MANUAL_TEST.md](MANUAL_TEST.md)。

## 14. ICE 模式切换 / 重连 `ice_switch_test.py`

```bash
python3 test/ice_switch_test.py     # 两个本地 daemon + 云端信令/coturn
```

| 覆盖内容 | 结果 |
|---|---|
| `get_ice_config` 回显 | 通过 |
| `switch_ice host`（offerer relay→host）：重建 PC、旧 relay 候选消失、对端 mode 不变 | 通过 |
| `switch_ice relay`（host→relay） | 通过 |
| answerer 被命令 `switch_ice relay`（对端发起重建） | 通过 |
| `reconnect`（offerer / answerer 双向，复用旧候选、mode 不变） | 通过 |
| 空闲 `switch_ice` → `not_in_call` | 通过 |

输出：`ice switch ok`。

## 15. 视频传输冒烟 `video_test.py`

```bash
python3 test/video_test.py          # 两个本地 daemon（合成设备）+ 云端信令
```

| 覆盖内容 | 结果 |
|---|---|
| `start_video`/`stop_video` → `video.state` | 通过 |
| `call` 后双方 `remote_video.started` + `connected` | 通过 |
| 远端帧计数日志（`Remote video frame #N WxH`）持续增长 | 通过 |

输出：`video test ok`。

## 16. 通话中视频开关 `video_toggle_test.py`

```bash
python3 test/video_toggle_test.py
```

| 覆盖内容 | 结果 |
|---|---|
| 默认关视频时 `call` 正常 | 通过 |
| offerer `video_on` → 对端 `remote_video.started`；answerer `video_on` 同理 | 通过 |
| 双向 `video_off` → 对端 `remote_video.stopped` | 通过 |
| 关后**再开**（T5/T6）正常 | 通过 |

输出：`video toggle ok`。

## 17. 视频参数控制 `video_params_test.py`

```bash
python3 test/video_params_test.py
```

| 覆盖内容 | 结果 |
|---|---|
| `video_on` → `video_switched(success)` | 通过 |
| `set_bitrate 500` / `set_framerate 15` / `set_degradation` → `video_switched` + `video_params` 回显 | 通过 |
| `video_off` → `video_switched(enabled=false)` + 对端 stopped | 通过 |
| 空闲 `set_bitrate` → `not_in_call` | 通过 |

输出：`video params ok`。

## 18. 视频统计 + 强制关键帧 `video_stats_test.py`

```bash
python3 test/video_stats_test.py
```

| 覆盖内容 | 结果 |
|---|---|
| `set_video_stats on 1000` 后周期 `video_stats`，`bytes_sent` 递增、`fps>0` | 通过 |
| `request_keyframe` → `keyframe_requested(success)` 且 `key_frames_encoded` 增长 | 通过 |
| `set_video_stats off` 后不再推送 | 通过 |
| 视频关闭时 `request_keyframe` → `failed` | 通过 |

输出：`video stats test ok`。

## 19. 视频设备选择 / 通话中切换 `video_device_test.py`

```bash
python3 test/video_device_test.py
```

| 覆盖内容 | 结果 |
|---|---|
| `get_video_devices` 枚举真实摄像头 + `synthetic`/`synthetic_hd` | 通过 |
| 空闲 `set_video_device` 选择真实摄像头 | 通过 |
| 真实摄像头 640x480 画面传输 | 通过 |
| 通话中切到 `synthetic_hd`（1280x720，**无重协商**） | 通过 |
| 真实↔合成来回切 10 次不崩 | 通过 |
| 切换后 `video_off` / 再 `video_on` | 通过 |
| `set_video_device nonsense` → `device_not_found` | 通过 |

输出：`video device ok`。

## 20. 双摄像头通话 `video_dual_camera_test.py`

```bash
python3 test/video_dual_camera_test.py     # 本机接 2 个 USB 摄像头
```

| 覆盖内容 | 结果 |
|---|---|
| 枚举到 2 个真实摄像头，两 daemon 各用一个 | 通过 |
| 双向收流（5 秒各 120 帧） | 通过 |
| 日志核对两边确实用了不同摄像头 | 通过 |
| 通话中切到空闲合成设备、切回原摄像头 | 通过 |
| 切到**对端正在用**的摄像头（驱动允许多开时成功；独占时 `failed` 并保留原设备） | 通过 |

输出：`dual camera test ok`。

## 21. 采集能力查询 + 档位切换 `video_profile_test.py`

```bash
python3 test/video_profile_test.py
```

| 覆盖内容 | 结果 |
|---|---|
| `get_video_device_capabilities` 返回 MJPG/YUYV + 各尺寸/帧率；`selectable` 标记（HD→MJPG、SD→YUYV） | 通过 |
| 空闲 `set_video_profile 1280 720 30` → 通话后远端收 1280x720 | 通过 |
| 通话中切 `640x480@30`（**无重协商**），远端分辨率跟着变 | 通过 |
| `set_video_profile 640 480 60` → `unsupported_profile` | 通过 |
| `get_video_status` 回显 device_id/device_name/capture_format | 通过 |

输出：`video profile ok`。

## 22. 摄像头热插拔 `video_hotplug_test.py`

```bash
SUDO_PASS=<密码> python3 test/video_hotplug_test.py    # 用 sysfs 解绑/重绑模拟拔插
```

| 覆盖内容 | 结果 |
|---|---|
| 通话中拔掉在用摄像头 → `video_device_lost` + `video_switched(false)` + 对端 `remote_video.stopped` | 通过 |
| `video_devices_changed(reason=device_removed)` | 通过 |
| 插回 → `device_added`（**不自动开视频**） | 通过 |
| 重新 `video_on` → 画面恢复 | 通过 |

输出：`hotplug test ok`。

## 23. 弱网自适应 `video_weaknet_test.py`

```bash
SUDO_PASS=<密码> python3 test/video_weaknet_test.py    # tc netem 只整形 dport 3478
```

| 场景 | 实测 | 结果 |
|---|---|---|
| S0 基线 | 29fps / 1700kbps / RTT 26ms | 通过 |
| S1 丢包 10% | lost 178、nack 155，码率降到 774kbps | 通过 |
| S2 带宽 500kbit | target 321kbps | 通过 |
| S3 延迟 200ms | RTT 424ms | 通过 |
| S4 恢复 | RTT 回落 27ms、码率回升 | 通过 |

输出：`weaknet test ok`。

## 24. 网络断开 `video_netdrop_test.py`

```bash
SUDO_PASS=<密码> python3 test/video_netdrop_test.py   # iptables 独立链 WEBRTC_TEST
```

| 场景 | 期望 | 结果 |
|---|---|---|
| N1 媒体瞬断 6s | `disconnected` → 恢复后**自动重连**、画面继续 | 通过 |
| N2 媒体断 45s | `failed` → 双方 `call_ended`、状态复位、进程存活 | 通过 |
| N2b 恢复后直接重呼（**不重启 coturn**） | 成功（端口窗口轮换避开旧 TURN 分配） | 通过 |
| N3 信令断（TCP 8888） | `signal_client.disconnected` → 恢复后重新 register + call 成功 | 通过 |

> 说明：长时断网按设计**挂断**（不做应用层自动重拨）；短时断网由 WebRTC ICE 自动恢复。
> 信令 `disconnected` 需等服务端 `ping_timeout`（默认 30s）判定。

输出：`netdrop test ok`。

## 25. 接口全覆盖 + 反复抗压 `stress_test.py`

```bash
python3 test/stress_test.py                       # 接口覆盖 + 双向抗压 ×5
python3 test/stress_test.py --interface-only      # 只跑接口覆盖
python3 test/stress_test.py --cycles 100          # 抗压 100 遍（曾实跑通过）
```

| 覆盖内容 | 结果 |
|---|---|
| **28 个命令**的成功事件 + 运行事实核对 | 通过 |
| **11 个 `ipc.error`**：usage / not_signed_in / already_in_call / not_in_call / data_channel_not_open / signal_send_failed / self_call / peer_offline / peer_busy / device_not_found / unsupported_profile | 通过 |
| 第三个"备用 daemon"覆盖 `name_taken` / `peer_busy` | 通过 |
| 运行事实：`video_stats` 字节递增、`target_bitrate`/`fps` 真受限、`key_frames_encoded` 增长、切设备后画面继续、旧 relay 候选消失 | 通过 |
| 双向（本地/云端 offerer）× call/leave、switch_ice、reconnect 各 5 遍，状态复位、进程存活 | 通过 |

输出：`全部通过 ✅（不崩 + 事件正确 + 不卡死）`。曾按 **每项 100 遍**实跑：846 PASS / 0 FAIL。

## 26. 媒体压测（3 摄像头）`video_media_stress_test.py`

```bash
SUDO_PASS=<密码> python3 test/video_media_stress_test.py
```

| 覆盖内容 | 结果 |
|---|---|
| 枚举 3 个真实摄像头 + 能力 | 通过 |
| 三摄像头依次切换（画面继续） | 通过 |
| 每个摄像头遍历 selectable 档位（320x240/352x288/640x480/960x540，共 12 次），远端都收到对应分辨率 | 通过 |
| 占用冲突：驱动允许多开时成功且 offerer 画面不受影响；`chmod 000` 造不可读设备 → 优雅失败保留原设备 | 通过 |
| 三摄像头 × 5 轮反复切换不崩 | 通过 |
| 合成 ↔ 真实切换 | 通过 |

输出：`media stress ok`。

## 27. 组合与命令风暴 `stress_combo_test.py`

```bash
SUDO_PASS=<密码> python3 test/stress_combo_test.py
```

| 类别 | 用例 | 结果 |
|---|---|---|
| 组合 | A1 视频中 `switch_ice`（host↔relay）画面继续 | 通过 |
| 组合 | A2 视频中 `reconnect` ×3 画面继续 | 通过 |
| 组合 | A3 切设备后立刻切档位 | 通过 |
| 组合 | A4 视频中热插拔（拔→关视频→插回→再开） | 通过 |
| 组合 | A5 `leave` 后立刻 `call` | 通过 |
| 风暴 | B1 `switch_ice` 三连发 → 最终 mode 正确、不崩 | 通过 |
| 风暴 | B2 `video_on` + `switch_ice` 冲突连发 → 仍 connected | 通过 |
| 风暴 | B3 `video_on/off` 抖动 → 最终 enabled=true | 通过 |
| 风暴 | B4 `set_bitrate` ×20 刷屏 → 回显最后值 | 通过 |
| 风暴 | B5 100 条非法命令 → 之后正常 `call` 仍成功 | 通过 |
| 风暴 | B6 **双客户端**：任一端可控制、事件双端可见、断开不影响另一端 | 通过 |

输出：`combo stress ok`。

## 说明

- 集成测试均依赖真 `signal_server_26_11_3/build/signaling_server`，各测试使用独立端口（9799/9899/9898/9897/9896/9895/9893/9892/9891/9890），避免互扰。
- IPC 层已纳入断言的改动：连接握手 + 命令清单、roster 带在线用户状态、`register` 指定/切换服务器、`signal_disconnect`/崩溃拆 PC、信令转发 `signal_msg`、运行时状态快照 `get_stats`。
- 错误事件分三类 scope：`ipc.error`（受理拒绝）、`engine.error`（引擎内部异步失败）、`signal_client.error`（信令服务器返回），完整命令 ↔ 事件/错误码汇总见 [IPC_PROTOCOL.md](IPC_PROTOCOL.md)。
- `get_stats` 返回的 `stats` 事件含三块：`data_channel`（状态/计数/积压，同步采集）、`transport` + `candidate_pair`（字节/DTLS/ICE/RTT，来自 PeerConnection GetStats）。
- 事件协议（后端 → 上层）为 JSON，字段以 `event` 路由；命令（上层 → 后端）保持纯文本。
- **多客户端 IPC**：支持最多 16 个 unix socket 客户端；任意客户端都可发命令（signaling 线程串行执行）。`ipc.connected` 只发新连接，**其余事件广播给全部客户端**，因此客户端会收到非自己命令触发的事件——UI 应以状态为准。
- **第 14 节起的前置条件**：云端信令服务器（8888）与云端 daemon 必须是**与本机同版本**的二进制（含视频/统计/设备/热插拔能力）；本地需有可用摄像头（第 15–19、21、25、27 节可显式 `set_video_device synthetic` 规避抢占）。
- **需要 sudo 的测试**（`SUDO_PASS=<密码>`）：第 22 节（sysfs 解绑摄像头）、第 23 节（tc netem）、第 24 节（iptables 独立链 `WEBRTC_TEST`）、第 26/27 节的热插拔部分。所有临时系统改动均在 `finally` 中恢复。
- **视频类测试**注意：同一摄像头不能被两个本地 daemon 同时采集；测试均显式指定设备（合成或分工），避免相互抢占。
- **known 环境交互**：长时断网后若在 TURN 分配寿命内重呼，可能撞 coturn `437 Mismatched allocation`；引擎采用端口窗口轮换（49152–65535 切 256 组、每次建 PC 换组、进程启动随机起点）规避，第 24 节 N2b 已验证不重启 coturn 也能重呼。
