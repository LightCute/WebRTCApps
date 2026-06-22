#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/dc_call_x64}"

DEFAULT_GN_ARGS='is_debug=true
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=false
rtc_include_pulse_audio=true
use_sysroot=true'

GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generating x64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/dc_call:dc_call \
  --args="${GN_ARGS}"

echo "=== Building dc_call (x64) ==="
ninja -C "${OUT_DIR}" dc_call

echo "Done: ${OUT_DIR}/dc_call"
