# 编译指南（稳定复现）

本文档给出**从零开始、稳定编译本仓库（`apps/`）WebRTC 相关工程**的完整步骤。

本仓库有两套构建体系：

| 工程 | 构建方式 | 依赖 |
|------|----------|------|
| `webrtc_client-26-11-4` | WebRTC **GN/ninja** 构建（`build_x64.sh`） | WebRTC 源码（精确 commit）+ 本仓库源码（third_party 已 vendor 进工程内） |
| `signal_server_26_11_3` / `peer_connection_client_26_11_3` | 独立 **g++** 构建（各自 `build.sh`） | websocketpp / asio（在 `third_party/`）/ jsoncpp（WebRTC 源码内） |

---

## 0. 环境要求

- Linux x86_64（Ubuntu 22.04 已验证；Debian/ChromeOS 亦可）
- `git`、`python3`、`g++`（或 clang）、`curl`
- 磁盘空间：WebRTC 源码 + 依赖 + 编译产物约 **40GB 以上**

---

## 1. 安装 depot_tools（一次性）

```bash
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PWD/depot_tools:$PATH"      # 提供 fetch / gclient / gn / ninja
```

> 建议把 `export PATH=...` 写进 `~/.bashrc`，后续每次都要用到 `gn`/`ninja`。

---

## 2. 拉取 WebRTC 源码（`--nohooks` 跳过初始全量同步，更快）

```bash
fetch --nohooks webrtc
cd src
```

---

## 3. 检出精确版本（务必锁定，否则 API 对不上）

```bash
git checkout 1c7365eedcee97547940638c6b13b3d862214473
```

---

## 4. 按该 commit 的 DEPS 同步所有依赖

```bash
gclient sync -D
```

> 这一步会按该 commit 的 `DEPS` 拉取全部第三方依赖（含 `chromium_revision 35639b034f`）。
> `-D` 删除 DEPS 中不再需要的依赖，保证干净。

完成后目录结构：

```
webrtc/
├── depot_tools/
└── src/                 # ← WebRTC 源码（HEAD = 1c7365...）
    ├── api/  pc/  rtc_base/  ...
    └── apps/            # ← 本仓库代码（见下一步）
```

---

## 5. 获取本仓库代码

把本仓库（远程仓库名 `WebRTCApps`）克隆到 WebRTC 源码树的 `src/` 下。**目录名随意**（`apps` 或 `WebRTCApps` 均可），`build_x64.sh` 会从实际路径动态推导 GN 标签，不再写死 `apps`：

```bash
cd src
git clone <你的 apps 仓库地址> apps          # 目录名任意：apps 或 WebRTCApps 都行
# 例如：git clone git@your-host:your-org/WebRTCApps.git
```

> 只要保证 `src/<任意目录>/webrtc_client-26-11-4/` 存在即可；`signal_server_26_11_3` 与它同级，共享 `third_party/`。

### 5.1 应用 WebRTC 源码补丁（必需）

本仓库的音频功能依赖一个针对锁定 commit 的 WebRTC 补丁（修复音频开启时 PC 重建后 Pulse 设备无法重新激活）：

```bash
cd src
git apply --check apps/patches/webrtc-1c7365e-pulse-adm-quit-reset.patch
git apply         apps/patches/webrtc-1c7365e-pulse-adm-quit-reset.patch
```

补丁说明、回退方式与换版本时的移植方法见 [patches/README.md](patches/README.md)。

---

## 6. 编译 webrtc_client（WebRTC 客户端 + 测试）

```bash
cd src/apps/webrtc_client-26-11-4
bash build_x64.sh
```

该脚本做了两件事：

1. `gn gen` 生成 x64 构建目录 `src/out/webrtc_client_x64`，GN 参数固定为：

   ```
   is_debug=true
   rtc_build_examples=false
   rtc_build_tools=false
   rtc_include_tests=false
   rtc_include_pulse_audio=true
   use_clang_modules=false
   use_sysroot=true
   ```

> 音频使用 **PulseAudio** 后端（`rtc_include_pulse_audio=true`，x64/arm64 一致）。
> 运行环境没有音频服务时，引擎会回退 dummy ADM（静音），视频功能不受影响。

2. `ninja -C out/webrtc_client_x64 webrtc_client` 编译。

产物（`src/out/webrtc_client_x64/`）：

| 产物 | 说明 |
|------|------|
| `webrtc_engine_main` | WebRTC 客户端主程序（unix socket 接入） |
| `webrtc_engine_smoke` | 冒烟测试 |
| `webrtc_engine_signaling_test` / `_call_test` / `_teardown_test` / `_multicall_test` | 引擎集成测试 |

> 说明：本工程的 `websocketpp` / `asio` 头文件已 vendor 到 `third_party/`，**无需**外部再装这两个库；唯一外部依赖是 WebRTC 源码本身。

---

## 7. 编译信令服务器（跑集成测试需要）

`webrtc_client` 的集成测试需要真信令服务器：

```bash
cd src/apps/signal_server_26_11_3
bash build.sh
# 产物：build/signaling_server、build/test_client 等
```

---

## 8. 运行测试

```bash
cd src/apps/webrtc_client-26-11-4

# C++ 引擎测试（6 个）
out/webrtc_client_x64/webrtc_engine_smoke            # 冒烟（创建/销毁 PC/DC）
out/webrtc_client_x64/webrtc_engine_validation_test  # 受理校验 + engine.error（无需网络）
bash test/integration_test.sh        # 信令
bash test/call_integration_test.sh   # 通话
bash test/teardown_integration_test.sh  # 挂断/掉线清理
bash test/multicall_integration_test.sh # 20 轮通话

# Python IPC 测试（5 个，走 unix socket 驱动真二进制）
python3 test/ipc_integration_test.py
python3 test/ipc_signaling_test.py
python3 test/ipc_disconnect_test.py
python3 test/ipc_multicall_test.py
python3 test/ipc_failure_test.py     # 命令失败返回（ipc.error）

# 音频（需要本机 PulseAudio/PipeWire 与可用麦克风）
python3 test/audio_test.py             # 设备枚举/选择、空闲+通话中开关、重建补轨、错误路径
python3 test/audio_processing_test.py  # AEC/NS/AGC 运行时切换（不重协商）
python3 test/audio_config_test.py      # 全量 APM 配置、源参数 pending、设备可用性/采样率
python3 test/audio_mute_volume_test.py # 软件静音 + 扬声器音量/静音
python3 test/audio_device_switch_test.py  # 空闲/通话中切录音设备
SUDO_PASS=<密码> python3 test/audio_hotplug_test.py  # 热插拔（USB unbind/bind）
python3 test/audio_stats_test.py       # 周期性音频统计 + 远端音频事件
```

全部通过即可确认编译产物可用。测试明细见 [webrtc_client-26-11-4/TEST.md](webrtc_client-26-11-4/TEST.md)。

---

## 9. 版本锁定与关键信息

| 项 | 值 |
|----|----|
| WebRTC 源码 commit | `1c7365eedcee97547940638c6b13b3d862214473` |
| chromium_revision（DEPS 锁定） | `35639b034f` |
| 构建系统 | depot_tools 的 gn + ninja |
| 构建目录 | `src/out/webrtc_client_x64` |
| 关键 GN 参数 | `is_debug=true`、`rtc_include_pulse_audio=true`、`use_clang_modules=false`、`use_sysroot=true` |
| 客户端依赖（已 vendor） | websocketpp、standalone Asio 头文件（`third_party/`） |
| WebRTC 源码补丁 | `patches/webrtc-1c7365e-pulse-adm-quit-reset.patch`（音频 PC 重建修复，**必须应用**） |

> **为什么必须锁 commit**：本仓库代码依赖该 WebRTC 版本的特定 API（`namespace webrtc`、`GetStats(RTCStatsCollectorCallback*)`、`make_ref_counted` 等）。换 commit 会导致 API 不匹配、编译失败。

---

## 10. 常见问题

- **`gn`/`ninja` 找不到**：depot_tools 未加进 `PATH`，重新 `export PATH="$PWD/depot_tools:$PATH"`。
- **`gclient sync` 报错**：确认 `git checkout 1c7365...` 已执行成功（`git rev-parse HEAD` 应等于该 hash）。
- **编译报 `websocketpp` 找不到**：检查 `apps/third_party/websocketpp` 是否存在；webrtc_client 与 signal_server 都从这里引用。
- **磁盘不足**：WebRTC 全量源码 + 构建产物较大，建议预留 40GB+。
