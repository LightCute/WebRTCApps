#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# 项目相对 WebRTC 源码根（src/）的路径，动态推导 GN 标签，不写死 "apps"：
# 这样本仓库无论被 clone 成 apps/ 还是仓库名 WebRTCApps/ 都能直接编译。
REL_DIR="$(realpath --relative-to="$SRC_ROOT" "$SCRIPT_DIR")"
ROOT_TARGET="//${REL_DIR}:webrtc_client"
OUT_DIR="${1:-${SRC_ROOT}/out/webrtc_client_x64}"

DEFAULT_GN_ARGS='is_debug=true
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=false
rtc_include_pulse_audio=false
use_clang_modules=false
use_sysroot=true'

GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generating x64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target="${ROOT_TARGET}" \
  --args="${GN_ARGS}"

echo "=== Building webrtc_client (x64) ==="
ninja -C "${OUT_DIR}" webrtc_client

echo "Done: ${OUT_DIR}/webrtc_engine_smoke"
