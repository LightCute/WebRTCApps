#!/usr/bin/env bash
set -u
# STUN 连通性测试：双 peer 走谷歌 STUN 的 srflx 候选建立通话。
# 注意：需要公网可达 + 两 peer 能通过 srflx 互通（两机不同网络，或测试机有公网 IP/NAT hairpin）。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_ROOT="$(cd "$ENGINE_DIR/../.." && pwd)"

SERVER="$ENGINE_DIR/../signal_server_26_11_3/build/signaling_server"
TEST_BIN="$SRC_ROOT/out/webrtc_client_x64/webrtc_engine_stun_test"
PORT=9879

"$SERVER" --port=$PORT --ping_interval=2 > /tmp/wc_stun_srv.log 2>&1 &
SRV=$!
sleep 1

"$TEST_BIN" $PORT > /tmp/wc_stun_test.log 2>&1
TEST_EXIT=$?

kill $SRV 2>/dev/null
wait 2>/dev/null

cat /tmp/wc_stun_test.log
exit $TEST_EXIT
