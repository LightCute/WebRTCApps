#!/usr/bin/env bash
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_ROOT="$(cd "$ENGINE_DIR/../.." && pwd)"

SERVER="$ENGINE_DIR/../signal_server_26_11_3/build/signaling_server"
TEST_BIN="$SRC_ROOT/out/webrtc_client_x64/webrtc_engine_teardown_test"
PORT=9898

"$SERVER" --port=$PORT --ping_interval=2 > /tmp/wc_td_srv.log 2>&1 &
SRV=$!
sleep 1

"$TEST_BIN" $PORT > /tmp/wc_td_test.log 2>&1
TEST_EXIT=$?

kill $SRV 2>/dev/null
wait 2>/dev/null

cat /tmp/wc_td_test.log
exit $TEST_EXIT
