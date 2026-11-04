#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

JSONCPP_INC="../../../third_party/jsoncpp/source/include"
JSONCPP_SRC="../../../third_party/jsoncpp/source/src/lib_json"
JSONCPP_FILES="$JSONCPP_SRC/json_reader.cpp $JSONCPP_SRC/json_value.cpp $JSONCPP_SRC/json_writer.cpp"

mkdir -p build

echo "=== build build/client_smoke ==="
g++ -std=c++17 -DASIO_STANDALONE \
    -I . -I ../websocketpp -I ../asio/asio/include \
    -I "$JSONCPP_INC" \
    ws_signaling_client.cc test/smoke_main.cc $JSONCPP_FILES \
    -o build/client_smoke -lpthread

echo "=== build build/client_harness ==="
g++ -std=c++17 -DASIO_STANDALONE \
    -I . -I ../websocketpp -I ../asio/asio/include \
    -I "$JSONCPP_INC" \
    ws_signaling_client.cc test/client_harness.cc $JSONCPP_FILES \
    -o build/client_harness -lpthread

echo "=== build build/dispatch_test ==="
g++ -std=c++17 -DASIO_STANDALONE \
    -I . -I ../websocketpp -I ../asio/asio/include \
    -I "$JSONCPP_INC" \
    ws_signaling_client.cc test/dispatch_test.cc $JSONCPP_FILES \
    -o build/dispatch_test -lpthread

echo "=== done ==="
