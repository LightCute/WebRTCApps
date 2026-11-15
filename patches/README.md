# WebRTC 源码补丁

这里的补丁作用于 **WebRTC 源码树**（`src/`），不是本仓库（`apps/`）的代码。

版本锁定：WebRTC commit `1c7365eedcee97547940638c6b13b3d862214473`
（与 `../README.md` 第 9 节一致；`git -C src rev-parse HEAD` 应等于该 hash）。

| 补丁 | 修改文件 | 作用 |
|---|---|---|
| `webrtc-1c7365e-pulse-adm-quit-reset.patch` | `modules/audio_device/linux/audio_device_pulse_linux.cc` | 在 `AudioDeviceLinuxPulse::Init()` 里复位 `quit_ = false`。原代码只在 `Terminate()` 置 `true`，而 ADM 实例会被媒体引擎复用（最后一个 PC 释放时 Terminate、新 PC 创建时再次 Init）；不复位会导致新的录音/播放线程在首次 Start 请求时直接退出，`StartRecording/StartPlayout` 各等 10s 超时，音频开启时 PC 重建后音频永不恢复。 |

## 应用

```bash
cd /path/to/webrtc/src
git apply --check apps/patches/webrtc-1c7365e-pulse-adm-quit-reset.patch
git apply         apps/patches/webrtc-1c7365e-pulse-adm-quit-reset.patch
```

也兼容 `patch -p1 < apps/patches/webrtc-1c7365e-pulse-adm-quit-reset.patch`。

## 回退

```bash
cd /path/to/webrtc/src
git apply --reverse apps/patches/webrtc-1c7365e-pulse-adm-quit-reset.patch
```

## 验证

```bash
cd apps/webrtc_client-26-11-4
python3 test/audio_test.py          # A3b: switch_ice host/relay 后音频自动恢复
```

## 更换 WebRTC commit 时

补丁的上下文按上述 commit 生成。换版本后先 `git apply --check`：

- 通过：直接应用；
- 冲突：按同样思路移植——在 `AudioDeviceLinuxPulse::Init()` 的
  `if (_initialized) return InitStatus::OK;` 之后、启动 `_ptrThreadRec/_ptrThreadPlay`
  之前，把 `quit_` 在 `mutex_` 保护下复位为 `false`。

WebRTC 源码被重新拉取（`gclient sync` / 重新 checkout）后，需要重新应用本补丁。
