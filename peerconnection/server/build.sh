#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/signaling_server}"

DEFAULT_GN_ARGS='is_debug=true
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=false
use_sysroot=true'
GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generating ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/server:peerconnection_server \
  --args="${GN_ARGS}"

echo "=== Building peerconnection_server ==="
ninja -C "${OUT_DIR}" peerconnection_server

echo "=== Done: ${OUT_DIR}/peerconnection_server ==="
