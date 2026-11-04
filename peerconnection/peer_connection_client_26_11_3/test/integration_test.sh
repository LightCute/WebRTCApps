#!/usr/bin/env bash
set -u
CLIENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="$CLIENT_DIR/../signal_server_26_11_3/build/signaling_server"
PEER="$CLIENT_DIR/../signal_server_26_11_3/build/test_client"
PORT=9499

rm -f /tmp/fifo_peer
mkfifo /tmp/fifo_peer

"$SERVER" --port=$PORT --ping_interval=2 > /tmp/it_srv.log 2>&1 &
SRV=$!
sleep 1

# harness 先注册 -> id 1
"$CLIENT_DIR/build/client_harness" $PORT > /tmp/it_harness.log 2>&1 &
HARN=$!
sleep 1

# peer 后注册 -> id 2
"$PEER" ws://127.0.0.1:$PORT < /tmp/fifo_peer > /tmp/it_peer.log 2>&1 &
PEER_PID=$!
exec 3>/tmp/fifo_peer
sleep 1

echo "register robot" >&3;        sleep 0.5   # peer -> id 2
echo "call 1 offer-sdp" >&3;     sleep 0.5   # peer 呼叫 harness(id1)
echo "candidate 1 cand 0 0" >&3; sleep 0.5   # peer 发 candidate
echo "leave" >&3;                sleep 1     # peer 离开 -> harness 收 bye

exec 3>&-
wait $HARN
HARN_EXIT=$?

kill $PEER_PID $SRV 2>/dev/null
wait 2>/dev/null

echo "=== harness log ==="; cat /tmp/it_harness.log
echo "=== peer log ==="; grep '<< ' /tmp/it_peer.log
echo "=== harness exit: $HARN_EXIT ==="
exit $HARN_EXIT
