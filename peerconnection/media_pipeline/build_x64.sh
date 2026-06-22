#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(dirname "$(dirname "$(dirname "$SCRIPT_DIR")")")"
OUT_DIR="$PROJ_DIR/out/media_pipeline_x64"

echo "=== media_pipeline_test build ==="
echo "Project dir: $PROJ_DIR"
echo "Output dir:  $OUT_DIR"

mkdir -p "$OUT_DIR"

cd "$PROJ_DIR"

echo "--- gn gen ---"
gn gen "$OUT_DIR" --args='target_cpu="x64" is_debug=true'

echo "--- ninja ---"
ninja -C "$OUT_DIR" media_pipeline

echo "=== Build complete ==="
echo "Run: $OUT_DIR/media_pipeline"
