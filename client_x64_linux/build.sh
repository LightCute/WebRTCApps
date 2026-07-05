#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
OUT_DIR="$PROJ_DIR/out/client_x64_linux"
echo "=== client_x64_linux build ==="
mkdir -p "$OUT_DIR"
cd "$PROJ_DIR"
gn gen "$OUT_DIR" --args='target_cpu="x64" is_debug=true rtc_include_tests=false rtc_build_tools=false'
ninja -C "$OUT_DIR" client_x64
echo "=== Build complete ==="
echo "Run: $OUT_DIR/client_x64"
