#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(dirname "$SCRIPT_DIR")"
OUT_DIR="$PROJ_DIR/out/client_arm64_linux"

echo "=== client_arm64_linux build ==="
echo "Project dir: $PROJ_DIR"
echo "Output dir:  $OUT_DIR"

mkdir -p "$OUT_DIR"

cd "$PROJ_DIR"

echo "--- gn gen ---"
gn gen "$OUT_DIR" --args='target_cpu="arm64" target_os="linux" is_debug=true rtc_include_tests=false rtc_build_tools=false'

echo "--- ninja ---"
ninja -C "$OUT_DIR" client_arm64_linux:client_arm64

echo "=== Build complete ==="
echo "Binary: $OUT_DIR/client_arm64"
