#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
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
  --root-target=//apps/webrtc_client-26-11-4:webrtc_client \
  --args="${GN_ARGS}"

echo "=== Building webrtc_client (x64) ==="
ninja -C "${OUT_DIR}" webrtc_client

echo "Done: ${OUT_DIR}/webrtc_engine_smoke"
