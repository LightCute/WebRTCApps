#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

JSONCPP_INC="../../third_party/jsoncpp/source/include"
JSONCPP_SRC="../../third_party/jsoncpp/source/src/lib_json"
JSONCPP_FILES="$JSONCPP_SRC/json_reader.cpp $JSONCPP_SRC/json_value.cpp $JSONCPP_SRC/json_writer.cpp"

mkdir -p build

echo "=== build build/messages_test ==="
g++ -std=c++17 -I . -I "$JSONCPP_INC" \
    messages.cc test/messages_test.cc $JSONCPP_FILES \
    -o build/messages_test

echo "=== build build/registry_test ==="
g++ -std=c++17 -I . \
    registry.cc test/registry_test.cc \
    -o build/registry_test

echo "=== build build/logger_test ==="
g++ -std=c++17 -I . \
    logger.cc test/logger_test.cc \
    -o build/logger_test

echo "=== build build/signaling_server ==="
g++ -std=c++17 -DASIO_STANDALONE \
    -I . -I ../third_party/websocketpp -I ../third_party/asio/include \
    -I "$JSONCPP_INC" \
    main.cc signaling_server.cc messages.cc registry.cc logger.cc $JSONCPP_FILES \
    -o build/signaling_server -lpthread

echo "=== build build/test_client ==="
g++ -std=c++17 -DASIO_STANDALONE \
    -I ../third_party/websocketpp -I ../third_party/asio/include \
    -I "$JSONCPP_INC" \
    test_client/test_client.cc $JSONCPP_FILES \
    -o build/test_client -lpthread

echo "=== done ==="
