#!/bin/bash
# Cross-compile rk_mpp_test for RK3588
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$SCRIPT_DIR/rk_mpp_test"

SDK=/home/light/sysroot_elf/aarch64-buildroot-linux-gnu_sdk-buildroot
SYSROOT=/home/light/sysroot_elf/rk3588-elf2-sysroot
CROSS=$SDK/bin/aarch64-linux-gcc

CFLAGS="-std=c11 -D_GNU_SOURCE -O2 -Wall"

$CROSS $CFLAGS \
  --sysroot="$SYSROOT" \
  -o "$OUT" \
  "$SCRIPT_DIR/main.c" \
  -ldl -lpthread -lm

echo "=== Built ==="
file "$OUT"
echo ""
echo "Deploy: scp $OUT root@192.168.6.226:/tmp/"
echo "Run:    ssh root@192.168.6.226 /tmp/rk_mpp_test"
