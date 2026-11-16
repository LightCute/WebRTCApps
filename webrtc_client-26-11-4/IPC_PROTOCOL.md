# 前端命令 ↔ 事件协议（完整参考）

本文档是前端（监控终端/UI）与 `webrtc_engine` 后端交互的**唯一协议约定**：每个命令的期望返回事件、失败时的错误码、以及所有事件的字段结构。

## 1. 通信方向与生命周期

- **命令（前端 → 后端）**：纯文本，一行一条，经 unix socket 发送。
- **事件（后端 → 前端）**：JSON，一行一个对象，以 `event` 字段路由。
- **多客户端**：支持最多 16 个 unix socket 客户端同时连接；**任意客户端都可发命令**（在 signaling 线程串行执行）。`ipc.connected` 只发给新连接的客户端，**其余所有事件广播给全部客户端**。因此客户端会收到并非自己命令触发的事件——UI 应以状态为准，而不是"等自己那条命令的回执"。

每条命令三段生命周期：

```
受理（同步，非法即时回 ipc.error） → 进行中 → 完成（异步，靠事件）
```

- 同步命令（`data_channel_msg` / `signal_msg` / `peer_connection_status`）：受理即出结果。
- 异步命令（`register` / `call` / `get_roster` / `get_stats` / `leave` / `signal_disconnect`）：受理通过后静默，完成靠成功/失败事件。

## 2. 事件词汇表（后端 → 前端）

### 2.1 握手

| 事件 | 字段 | 含义 |
|---|---|---|
| `ipc.connected` | `commands`（string[]） | 前端连接成功，附全部可用命令清单 |

### 2.2 信令

| 事件 | 字段 | 含义 |
|---|---|---|
| `signal_client.signed_in` | `id`（int） | 注册成功，拿到服务器分配的 id |
| `signal_client.disconnected` | —— | 与信令服务器断开（主动断开 / 服务器崩溃 / 连接掉线） |
| `signal_client.roster` | `peers`（[{`id`,`name`,`state`}]） | 在线列表（state: idle/busy） |
| `signal_client.incoming_call` | `from`（int） | 收到对端来电 |
| `signal_client.message` | `from`,`text` | 收到经信令转发的文本消息 |
| `signal_client.call_ended` | `from`（int） | 通话结束（对端 leave 或掉线） |
| `signal_client.local_sdp` | `type`,`sdp` | 本地生成的 SDP（offer/answer） |
| `signal_client.remote_sdp` | `type`,`sdp` | 远端发来的 SDP |
| `signal_client.local_candidate` | `candidate`,`mid`,`index` | 本地 ICE 候选 |
| `signal_client.remote_candidate` | `candidate`,`mid`,`index` | 远端 ICE 候选 |

### 2.3 PeerConnection / DataChannel

| 事件 | 字段 | 含义 |
|---|---|---|
| `peer_connection.state` | `state`（string） | PC 状态变化（connected/failed/closed 等） |
| `peer_connection_status` | `signed_in`,`my_id`,`connected`,`data_channel_open`,`peer_id` | 状态查询（同步返回全量状态） |
| `data_channel.state` | `state`（string） | DataChannel 状态变化（open/closed 等） |
| `data_channel_msg` | `text` | 收到对端经数据通道发来的消息 |

### 2.4 运行时状态快照

| 事件 | 字段 | 含义 |
|---|---|---|
| `stats` | `data_channel`,`transport`,`candidate_pair`,`local_candidates[]`,`remote_candidates[]`,`inbound_rtp[]`,`outbound_rtp[]`,`remote_inbound_rtp[]`,`data_channels_opened`,`data_channels_closed` | `get_stats` 触发，采集 DataChannel 计数 + GetStats 传输/候选对/全部候选（`local_candidates[]`/`remote_candidates[]` 各含 `ip/port/protocol/candidate_type/selected`）/RTP |

### 2.5 错误

| 事件 | 字段 | 含义 |
|---|---|---|
| `ipc.error` | `code`,`msg` | 受理拒绝（参数/状态不对，命令未执行） |
| `engine.error` | `code`,`msg` | 引擎内部异步失败 |
| `signal_client.error` | `code`,`msg` | 信令服务器返回错误 / 连接失败 |

### 2.6 ICE 模式切换 / 重连

| 事件 | 字段 | 含义 |
|---|---|---|
| `ice_config` | `mode`,`stun`,`turn`,`turn_user`,`turn_pass` | 当前 ICE 配置回显（`get_ice_config` / `set_ice_transports` / `switch_ice` 后） |
| `signal_client.switch_ice` | `from`（int） | 对端发来 switch_ice 请求（本端是被动方：重建 PC 但 mode 不变） |
| `signal_client.reconnect` | `from`（int） | 对端发来 reconnect 请求（本端是被动方：offerer 发起 ICE 重启） |
| `ice_switched` | `result`（`success`/`failed`） | 模式切换完成：重建后是否重新连通 |
| `reconnected` | `result`（`success`/`failed`） | ICE 重连完成：复用旧候选重新协商是否完成 |

### 2.7 视频（开关 / 参数 / 设备）

| 事件 | 字段 | 含义 |
|---|---|---|
| `video.state` | `enabled`（bool） | 本端视频期望状态（`start_video`/`stop_video`/`video_on`/`video_off` 受理即发） |
| `video_switched` | `enabled`,`result` | 视频操作完成/失败（`video_on`/`video_off` 重协商完成，或参数设置结果） |
| `video_params` | `enabled`,`in_call`,`device_id`,`device_name`,`device_synthetic`,`capture_width`,`capture_height`,`capture_fps`,`capture_format`,`width`,`height`,`fps`,`bitrate_kbps`,`degradation` | 完整视频状态回显（`get_video_status`，以及设置档位/切设备/参数变化后自动推送）。`device_*` 当前设备；`capture_*` 采集档位 + 实际生效像素格式；`width/height/fps/bitrate_kbps/degradation` 是**编码**参数 |
| `remote_video.started` | —— | 收到远端视频轨（对端开始发视频） |
| `remote_video.stopped` | —— | 远端视频轨移除（对端停止发视频） |
| `video_devices` | `current`,`devices[]` | 设备列表（`id`/`name`/`synthetic`/`width`/`height`/`fps`）+ 当前设备 |
| `video_device` | `id`,`name`,`result` | 设备切换结果（`failed` 时保留原设备继续传） |
| `video_device_capabilities` | `id`,`formats[]` | 设备采集能力：`formats[].pixel_format` + `formats[].sizes[]{width,height,fps[],selectable}`（含 MJPG / YUYV）。`selectable=true` 表示请求该尺寸时实际会用这个格式（WebRTC 规则：>640x480 优先 MJPG，否则 YUYV），前端应只允许 `selectable=true` 的组合 |
| `video_profile` | `width`,`height`,`fps`,`result` | 采集档位设置结果（`failed` 时保留原档位） |
| `video_devices_changed` | `devices[]`,`current`,`reason` | **热插拔**：设备集合变化（`reason`：`device_added` / `device_removed`）。仅通知，不自动切换 |
| `video_device_lost` | `id` | **当前正在使用的摄像头被拔出**；视频通道已自动关闭（随后会有 `video_switched`=`enabled:false`，对端收 `remote_video.stopped`） |
| `video_stats` | `width`,`height`,`fps`,`target_bitrate`,`bytes_sent`,`packets_sent`,`packets_lost`,`jitter_ms`,`rtt_ms`,`available_outgoing_bitrate`,`quality_limitation`,`frames_encoded`,`key_frames_encoded`,`pli_count`,`nack_count` | **周期性视频统计**（`set_video_stats on` 后按 interval 推送；仅通话中且视频开启时） |
| `keyframe_requested` | `result` | `request_keyframe` 的结果（视频未开启/无发送者时 `failed`） |

### 2.8 音频（开关 / 设备 / 处理）

| 事件 | 字段 | 说明 |
|---|---|---|
| `audio.state` | `enabled`（bool） | 本端音频期望状态（`audio_on`/`audio_off` 受理即发） |
| `audio_switched` | `enabled`,`result` | 音频开关完成/失败：空闲时立即 success；通话中为 AddTrack/RemoveTrack 重协商完成 |
| `audio_status` | `enabled`,`device_id`,`device_name`,`in_call`,`aec`,`ns`,`agc`,`has_device_module` | 完整音频状态回显（`get_audio_status`）。`device_id` 为空 = 系统默认设备；`has_device_module=false` 表示平台 ADM 不可用、已回退 dummy（无真实采集） |
| `audio_devices` | `current`,`devices[]` | 录音设备列表：`devices[].index`/`id`（稳定 Pulse source name，如 `alsa_input.usb-...`；默认别名=`default`）/`name`（描述，可能重名；ADM 已过滤 monitor 伪设备）+ `available`（`yes`/`no`/`unknown`，端口是否真的可用）+ `hardware_sample_rate`/`hardware_channels`（libpulse 查询，0=未知）。`current` 为选中设备的稳定 id，空 = 系统默认 |
| `audio_device` | `id`,`name`,`result` | 录音设备选择结果（空闲记住；通话中立即切换，失败保留原设备） |
| `audio_processing` | `aec`,`ns`,`agc` | AEC/降噪/AGC 运行时开关回显（`set_audio_processing`，不重协商；旧接口） |
| `audio_config` | `processing`（全量 APM 配置树）、`source`（AudioOptions）、`pending_source[]`、`device` | 完整音频配置（`get_audio_config` / `set_audio_config`）。`device` 含 `device_id/device_name/in_call/has_device_module/mic_muted/speaker_volume(_available)/speaker_muted(_available)/recording_sample_rate/playout_sample_rate/stereo_recording/stereo_playout` |
| `audio_devices_changed` | `devices[]`,`current`,`reason` | **热插拔**：`reason`=`device_added`/`device_removed`。仅通知，不自动切换 |
| `audio_device_lost` | `id` | **当前录音设备被拔出**；若通话中音频开启，引擎已自动关闭音频通道（随后 `audio_switched`=`enabled:false`，对端收 `remote_audio.stopped`） |
| `audio_stats` | `enabled`,`in_call`,`recv`,`send`,`remote`,`apm` | **周期性音频统计**（`set_audio_stats on`）。`recv`：`codec/packets_received/bytes_received/packets_lost/jitter_ms/audio_level`；`send`：`codec/packets_sent/bytes_sent/target_bitrate`；`remote`：`rtt_ms/fraction_lost`；`apm`：`residual_echo_likelihood/echo_return_loss/delay_median_ms` |
| `remote_audio.started` / `remote_audio.stopped` | —— | 对端开始 / 停止发音频轨 |

## 3. 错误码词汇表

### 3.1 `ipc.error` —— 受理拒绝

| code | 含义 | 触发命令 |
|---|---|---|
| `usage` | 参数缺失/非法 | `register`（name 空）、`call`（peer<=0）、`signal_msg`（peer<=0）、`set_ice_transports`/`switch_ice`（mode 空） |
| `not_signed_in` | 未登录信令服务器 | `call` / `get_roster` / `leave` / `signal_disconnect` |
| `already_in_call` | 已在通话中 | `call` / `set_ice_transports` |
| `not_in_call` | 不在通话中 | `leave` / `switch_ice` / `reconnect` |
| `device_not_found` | 视频设备 id 不存在 | `set_video_device` / `get_video_device_capabilities` |
| `unsupported_profile` | 该设备不支持此采集档位 | `set_video_profile` |
| `data_channel_not_open` | 数据通道未打开（未通话） | `data_channel_msg` |
| `signal_send_failed` | 信令消息发送失败（未连服务器） | `signal_msg` |
| `self_call` | 呼叫自己的 id | `call` |
| `peer_offline` | 对端不在线（call 前 roster 校验） | `call` |
| `peer_busy` | 对端忙线（call 前 roster 校验） | `call` |

### 3.2 `engine.error` —— 引擎内部异步失败

| code | 含义 |
|---|---|
| `peer_connection_factory_failed` | PeerConnectionFactory 创建失败 |
| `peer_connection_failed` | PeerConnection 创建失败 |
| `data_channel_failed` | DataChannel 创建失败 |
| `sdp_create_failed` | CreateOffer / CreateAnswer 失败 |
| `set_local_desc_failed` | SetLocalDescription 失败 |
| `set_remote_desc_failed` | SetRemoteDescription 失败 / 远端 SDP 解析失败 |

### 3.3 `signal_client.error` —— 信令错误

| code | 含义 |
|---|---|
| `connect` | 无法连接信令服务器（服务器不可达 / 连接超时 / 拒连） |
| `name_taken` | 名字已被占用（register） |
| `peer_busy` | 对方忙线（call） |
| `peer_offline` | 对方不在线（call / signal_msg） |

## 4. 命令 ↔ 事件总表

| 命令 | 受理拒绝码（ipc.error） | 成功事件 | 失败事件 |
|---|---|---|---|
| `register <name> [ip] [port]` | `usage` | `signal_client.signed_in`（id） | `signal_client.error`（connect / name_taken）、`signal_client.disconnected` |
| `call <peer_id>` | `usage` / `not_signed_in` / `already_in_call` | `signal_client.local_sdp` → `peer_connection.state`=connected → `data_channel.state`=open | `signal_client.error`（offline / busy）、`engine.error`（§3.2）、`peer_connection.state`=failed |
| `data_channel_msg <text>` | `data_channel_not_open` | 无异步事件（bool true = 已入 SCTP 发送队列） | 无（送达确认需应用层 ack，未实现） |
| `signal_msg <peer_id> <text>` | `usage` / `signal_send_failed` | 无异步事件（已发服务器；对端收到 `signal_client.message`） | `signal_client.error`（offline） |
| `leave` | `not_signed_in` / `not_in_call` | `signal_client.call_ended`（双方） | 无（已受理即触发 call_ended） |
| `get_roster` | `not_signed_in` | `signal_client.roster` | 无（已受理即回 roster；连接中断走 disconnected） |
| `get_stats` | 无（无前置） | `stats` | 无（总返回；PC 为空回全空快照） |
| `peer_connection_status` | 无（纯同步查询） | `peer_connection_status`（同步） | 无（总成功） |
| `signal_disconnect` | `not_signed_in` | `signal_client.disconnected` | 无（已受理即触发 disconnected） |
| `set_ice_transports <mode>` | `usage` / `already_in_call` | `ice_config` | 无（空闲时改下次 call 的 mode，不触发连接） |
| `switch_ice <mode>` | `usage` / `not_in_call` | `ice_config` → 重建后 `ice_switched`（success/failed）；对端收 `signal_client.switch_ice` | `ice_switched`=failed |
| `reconnect` | `not_in_call` | 协商后 `reconnected`（success/failed）；对端收 `signal_client.reconnect` | `reconnected`=failed |
| `get_ice_config` | 无（纯查询） | `ice_config` | 无（总返回） |
| `start_video` / `stop_video` | `already_in_call` | `video.state` | 无（只设下次 call 的开关） |
| `video_on` / `video_off` | `not_in_call` | `video.state` → 重协商后 `video_switched` | `video_switched`=failed |
| `set_bitrate <kbps>` / `set_framerate <fps>` / `set_degradation <mode>` | `usage` / `not_in_call` | `video_switched` + `video_params` | `video_switched`=failed |
| `get_video_status` | 无（纯查询） | `video_params` | 无（总返回） |
| `get_video_devices` | 无（纯查询） | `video_devices` | 无（总返回；无摄像头时只有合成设备） |
| `set_video_device <id>` | `usage` / `device_not_found` | `video_device`（空闲记住 / 通话中立即切换，不重协商） | `video_device`=failed（保留原设备） |
| `get_video_device_capabilities <id>` | `usage` / `device_not_found` | `video_device_capabilities` | 无（总返回） |
| `set_video_profile <w> <h> <fps>` | `usage` / `unsupported_profile` | `video_profile`（空闲记住 / 通话中立即切换，不重协商） | `video_profile`=failed（保留原档位） |
| `set_video_stats <on\|off> [interval_ms]` | `usage` | `video_stats`（周期推送，interval 默认 2000，clamp [500,10000]） | 无 |
| `request_keyframe` | `not_in_call` | `keyframe_requested`（success/failed） | `keyframe_requested`=failed |
| `get_audio_devices` | 无（纯查询） | `audio_devices` | 无（总返回；Pulse 不可用时为空列表） |
| `set_audio_device <id\|name>` | `usage` / `device_not_found` | `audio_device`（空闲记住；通话中立即切换，短暂采集中断、不重协商）。优先按稳定 id 匹配，兼容按描述名匹配 | `audio_device`=failed（保留原设备） |
| `audio_on` / `audio_off` | 无 | `audio.state` → `audio_switched`（空闲立即 success；通话中重协商后） | `audio_switched`=failed |
| `get_audio_status` | 无（纯查询） | `audio_status` | 无（总返回） |
| `set_audio_processing <aec\|ns\|agc> <on\|off>` | `usage` | `audio_processing`（旧接口，等价 `set_audio_config`，立即生效） | 无 |
| `get_audio_config` | 无（纯查询） | `audio_config` | 无（总返回） |
| `set_audio_config <key> <value>` | `usage`（未知 key/非法值） | `audio_config`（APM key 立即生效；`source.*` 进入 `pending_source`，下次建音轨生效；`mic_muted`/`speaker_volume`/`speaker_muted` 立即生效） | 无 |
| `set_audio_stats <on\|off> [interval_ms]` | `usage` | `audio_stats`（周期推送，interval 默认 2000，clamp [500,10000]） | 无 |
| `quit` | 无（总执行） | 无事件（unix socket 关闭即退出） | 无（总执行） |

## 5. 说明

1. **`data_channel_msg` 送达语义**：命令方向是「发送」；事件 `data_channel_msg`（含 `text` 字段）是「收到对端消息」时的上报，二者同名但方向相反。`Send` 返回 true 只代表消息进入 SCTP 发送队列，不代表对端收到；确送达需对端回一条应用层 ack（当前未实现）。
2. **异步命令完成看事件**：`register` / `call` / `get_roster` / `get_stats` / `leave` / `signal_disconnect` 的「完成」以成功事件为准；受理通过只表示命令合法、已进入异步流程。
3. **错误码 vs WebRTC 状态**：错误码是「本次命令为什么失败」的判决（应用层定义，现场由 WebRTC 状态推导）；WebRTC 自身状态（`PeerConnectionState` / `DataState` 等）通过 `peer_connection.state` / `data_channel.state` 事件与 `get_stats` 暴露，二者不冲突。
4. **`signal_client.disconnected` 时机**：连接建立后任何时刻都可能出现（服务器崩溃、`signal_disconnect`、网络掉线），与 `register` 无关地作为「已掉线」信号；`register` 阶段连接失败则走 `signal_client.error` code=`connect`。
5. **音频能力（第二期）**：
   - `audio_on/off`：空闲记住 + 通话中实时切换；`set_audio_device` 空闲记住、通话中立即切换（短暂采集中断，不重协商；失败回滚原设备）。
   - `set_audio_config` 的 key 与 `webrtc::AudioProcessing::Config` 点号路径一致（如 `noise_suppression.level`、`gain_controller1.mode`、`gain_controller2.fixed_digital.gain_db`、`pre_amplifier.fixed_gain_factor`、`high_pass_filter.enabled`、`transient_suppression.enabled`、`capture_level_adjustment.*`）；**pipeline 字段不暴露**。别名：`aec`/`ns`/`ns_level`/`agc`/`agc_mode`/`hpf`/`transient`/`agc2`。冲突自动裁决：开 AGC2 自动关 AGC1；开 `capture_level_adjustment` 自动关 `pre_amplifier`；AEC 强制 HPF。
   - `source.*`（`stereo_swapping`、`jitter_max_packets`、`jitter_fast_accelerate`、`jitter_min_delay_ms`、`init_recording_on_send`）存 pending，下次建音轨（`audio_on` / 下次 call）生效，`pending_source` 回显。
   - `mic_muted` 是**软件静音**（采集帧清零，RTP 继续发静音包，只影响本进程）；`speaker_volume`/`speaker_muted` 走 ADM，是 **Pulse sink 级（系统范围）**，会影响同一 sink 的其他应用。
   - 热插拔策略 A：当前录音设备被拔出 → 关音频轨 + 重协商 + `audio_device_lost`；插回只发 `audio_devices_changed`，不自动切回。
   - 设备名可含空格（Pulse source 描述），`set_audio_device` 取整行剩余内容。
   - 依赖 WebRTC 补丁 `patches/webrtc-1c7365e-pulse-adm-quit-reset.patch`：否则音频开启时 `switch_ice`/PC 重建后音频无法恢复。
