#!/bin/bash
# ── 陪护机器人 UI 启动脚本 ──
# 确保所有子进程 (WebRTC daemon, AI, voice chat) 工作目录一致

# .desktop 启动时不 source .profile — 手动补全环境
if [ -f "$HOME/.profile" ]; then
    . "$HOME/.profile"
fi
export PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

cd /home/elf/webrtc_monitor || { echo "ERROR: /home/elf/webrtc_monitor not found"; exit 1; }
export WEBRTC_RUNTIME_DIR=/tmp/webrtc_runtime
mkdir -p "$WEBRTC_RUNTIME_DIR"
exec ./ui_rk "$@"
