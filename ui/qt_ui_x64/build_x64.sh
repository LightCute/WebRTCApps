#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build-x64"
PRO_FILE="$PROJECT_DIR/ui_x64.pro"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Running qmake ==="
qmake "$PRO_FILE"

echo "=== Building ==="
make -j$(nproc)

if [ -f ui_x64 ]; then
    echo "=== Build SUCCESS ==="
    file ui_x64
    ls -la ui_x64
else
    echo "=== Build FAILED ==="
    exit 1
fi
