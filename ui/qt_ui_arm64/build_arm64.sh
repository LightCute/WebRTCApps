#!/usr/bin/env bash
set -euo pipefail

SDK=/home/light/sysroot_elf/aarch64-buildroot-linux-gnu_sdk-buildroot
SYSROOT=$SDK/aarch64-buildroot-linux-gnu/sysroot
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build-arm64"
PRO_FILE="$PROJECT_DIR/ui_rk.pro"

export PATH=$SDK/bin:$SDK/sbin:$PATH

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Running qmake ==="
$SDK/bin/qmake "$PRO_FILE" -spec devices/linux-buildroot-g++ || $SDK/bin/qmake "$PRO_FILE"

echo "=== Building ==="
make -j$(nproc)

if [ -f ui_rk ]; then
    echo "=== Build SUCCESS ==="
    file ui_rk
    ls -la ui_rk
else
    echo "=== Build FAILED ==="
    exit 1
fi
