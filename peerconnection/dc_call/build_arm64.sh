#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/dc_call_arm64}"

DEFAULT_GN_ARGS='target_os="linux"
target_cpu="arm64"
is_debug=true
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=false
rtc_include_pulse_audio=true
rtc_use_h264=true
use_glib=true'

GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generating ARM64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/dc_call:dc_call \
  --args="${GN_ARGS}"

echo "=== Building dc_call (ARM64) ==="
ninja -C "${OUT_DIR}" dc_call

echo "Done: ${OUT_DIR}/dc_call"
