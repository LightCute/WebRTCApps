#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/client_arm64_linux}"

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

echo "=== Generate ARM64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/client_arm64_linux:client_arm64 \
  --args="${GN_ARGS}"

echo "=== Build client_arm64 ==="
ninja -C "${OUT_DIR}" apps/client_arm64_linux:client_arm64

echo "=== Build complete ==="
echo "Binary: ${OUT_DIR}/client_arm64"
