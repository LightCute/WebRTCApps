#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/dec_test_arm64}"

DEFAULT_GN_ARGS='target_os="linux" target_cpu="arm64" is_debug=true rtc_build_examples=false rtc_build_tools=false rtc_include_tests=false rtc_include_pulse_audio=false use_sysroot=true'
GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generating arm64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/webrtc_mpp_rga_mix/dec_test:dec_test \
  --args="${GN_ARGS}"

echo "=== Building arm64 dec_test ==="
ninja -C "${OUT_DIR}" dec_test

echo "=== Done: ${OUT_DIR}/dec_test ==="
file "${OUT_DIR}/dec_test"
ls -lh "${OUT_DIR}/dec_test"
