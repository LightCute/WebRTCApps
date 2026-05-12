#!/usr/bin/env bash
# Cross-compile MPP tests for RK3588 ARM64
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/arm64_mpp_test}"

GN_ARGS='target_os="linux"
target_cpu="arm64"
is_debug=true
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=true
rtc_include_pulse_audio=true
rtc_use_h264=true
use_glib=true'

echo "=== Generating ARM64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/client:mpp_codec_test \
  --root-target=//apps/peerconnection/client:mpp_codec_debug \
  --args="${GN_ARGS}"

echo "=== Building mpp_codec_debug (with printf stepping) ==="
ninja -C "${OUT_DIR}" mpp_codec_debug

echo "=== Building mpp_codec_test ==="
ninja -C "${OUT_DIR}" mpp_codec_test

echo ""
echo "=== Binaries ==="
file "${OUT_DIR}/mpp_codec_debug"
file "${OUT_DIR}/mpp_codec_test"
echo ""
echo "Deploy debug: scp ${OUT_DIR}/mpp_codec_debug root@192.168.6.226:/tmp/"
echo "Run debug:    ssh root@192.168.6.226 /tmp/mpp_codec_debug"
