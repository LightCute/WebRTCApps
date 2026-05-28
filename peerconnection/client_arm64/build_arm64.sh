#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
# 自定义ARM64输出目录
OUT_DIR="${1:-${SRC_ROOT}/out/arm64_peerconnection_client}"

# 🔥 核心：ARM64交叉编译专属参数（替换原x64参数）
DEFAULT_GN_ARGS='target_os="linux" target_cpu="arm64"
is_debug=false
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=false
rtc_include_pulse_audio=false
use_sysroot=true
rtc_use_h264=true
ffmpeg_branding="Chrome"'

GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== 生成 ARM64 构建文件：${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/client_arm64:peerconnection_client \
  --args="${GN_ARGS}"

echo "=== 编译 ARM64 版本客户端 ==="
ninja -C "${OUT_DIR}" apps_peerconnection_client_arm64

echo "✅ 编译完成！ARM64可执行文件：${OUT_DIR}/apps_peerconnection_client_arm64"