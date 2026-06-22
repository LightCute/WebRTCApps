#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/video_rga_mpp_enc_arm64}"

DEFAULT_GN_ARGS='target_os="linux" target_cpu="arm64" is_debug=true rtc_build_examples=false rtc_build_tools=false rtc_include_tests=false rtc_include_pulse_audio=false use_sysroot=true'
GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generating arm64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/video_rga_mpp/enc:video_rga_mpp_enc \
  --args="${GN_ARGS}"

echo "=== Building arm64 video_rga_mpp_enc ==="
ninja -C "${OUT_DIR}" video_rga_mpp_enc

echo "=== Done: ${OUT_DIR}/video_rga_mpp_enc ==="
file "${OUT_DIR}/video_rga_mpp_enc"
ls -lh "${OUT_DIR}/video_rga_mpp_enc"
