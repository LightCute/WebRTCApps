#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/apps_peerconnection_client}"
TARGET="//apps/peerconnection/client:unix_socket_server_test"

echo "Regenerating ${OUT_DIR} (adding test target)"
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/client:peerconnection_client \
  --root-target="${TARGET}" \
  --args='is_debug=true
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=true
rtc_include_pulse_audio=true
use_sysroot=true'

echo "Building ${TARGET}"
ninja -C "${OUT_DIR}" unix_socket_server_test

echo ""
echo "Running unix_socket_server_test"
"${OUT_DIR}/unix_socket_server_test"
