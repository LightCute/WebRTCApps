#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# 项目相对 WebRTC 源码根（src/）的路径，动态推导 GN 标签，适配任意仓库名。
REL_DIR="$(realpath --relative-to="$SRC_ROOT" "$SCRIPT_DIR")"
ROOT_TARGET="//${REL_DIR}:webrtc_client"
OUT_DIR="${1:-${SRC_ROOT}/out/webrtc_client_arm64}"

# 与 x64 构建保持一致，仅改 target_cpu=arm64、is_debug=false（部署用 release）。
# use_sysroot=true 使用 WebRTC 预置的 Debian bullseye arm64 sysroot（glibc 2.31，
# 可在板子 glibc 2.38 上运行），由仓库内自带的 clang 交叉编译，无需 sysroot_elf。
DEFAULT_GN_ARGS='target_os="linux"
target_cpu="arm64"
is_debug=false
rtc_build_examples=false
rtc_build_tools=false
rtc_include_tests=false
rtc_include_pulse_audio=true
use_clang_modules=false
use_sysroot=true'

GN_ARGS="${GN_ARGS:-${DEFAULT_GN_ARGS}}"

echo "=== Generating arm64 build: ${OUT_DIR} ==="
gn gen "${OUT_DIR}" \
  --root="${SRC_ROOT}" \
  --root-target="${ROOT_TARGET}" \
  --args="${GN_ARGS}"

echo "=== Building webrtc_client (arm64) ==="
ninja -C "${OUT_DIR}" webrtc_client

echo "Done: ${OUT_DIR}/webrtc_engine_main"
