#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/audio_test_x64}"

DEFAULT_GN_ARGS='target_os="linux" target_cpu="x64"
is_debug=false
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=false'

GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generate x64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/audio_test:audio_test \
  --args="${GN_ARGS}"

echo "=== Build x64 audio_test ==="
ninja -C "${OUT_DIR}"

echo ""
echo "Done! Binary: ${OUT_DIR}/audio_test"
echo "Run locally: ${OUT_DIR}/audio_test --duration=5"
