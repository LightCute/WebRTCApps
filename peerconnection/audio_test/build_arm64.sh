#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/audio_test_arm64}"

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
  --root-target=//apps/peerconnection/audio_test:audio_test \
  --args="${GN_ARGS}"

echo "=== Build ARM64 audio_test ==="
ninja -C "${OUT_DIR}"

echo ""
echo "Done! Binary: ${OUT_DIR}/audio_test"
echo "Run on RK3588: ${OUT_DIR}/audio_test --duration=15"
