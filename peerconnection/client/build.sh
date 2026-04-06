#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${1:-${SRC_ROOT}/out/apps_peerconnection_client}"

DEFAULT_GN_ARGS='is_debug=true rtc_build_examples=false rtc_build_tools=false rtc_include_tests=true rtc_include_pulse_audio=true'
GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "Generating ${OUT_DIR}"
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target=//apps/peerconnection/client:peerconnection_client \
  --args="${GN_ARGS}"

echo "Building peerconnection_client"
ninja -C "${OUT_DIR}" apps_peerconnection_client

echo "Built executable: ${OUT_DIR}/apps_peerconnection_client"
