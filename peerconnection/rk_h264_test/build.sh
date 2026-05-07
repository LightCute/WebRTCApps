#!/bin/bash
# Cross-compile rk_h264_test for RK3588
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MPP_TOP="$(cd "$SCRIPT_DIR/../mpp" && pwd)"
OUT="$SCRIPT_DIR/rk_h264_test"

SDK=/home/light/sysroot_elf/aarch64-buildroot-linux-gnu_sdk-buildroot
SYSROOT=/home/light/sysroot_elf/rk3588-elf2-sysroot
CROSS=$SDK/bin/aarch64-linux-gcc

INCLUDES="
  -I${MPP_TOP}/inc
  -I${MPP_TOP}/osal/inc
  -I${MPP_TOP}/utils
  -I${MPP_TOP}/mpp/base/inc
"

$CROSS -std=c11 -D_GNU_SOURCE -O2 \
  --sysroot="$SYSROOT" \
  $INCLUDES \
  -o "$OUT" \
  "$SCRIPT_DIR/main.c" \
  "$MPP_TOP/utils/camera_source.c" \
  -lrockchip_mpp -lrt -lpthread -lm

echo "=== Built ==="
file "$OUT"
echo ""
echo "Deploy: scp $OUT root@192.168.6.226:/tmp/"
