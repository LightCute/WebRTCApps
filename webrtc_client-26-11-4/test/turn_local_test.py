#!/usr/bin/env python3
"""本地 TURN relay 跑通测试。

两个本地 daemon（offerer + answerer）都走 relay 模式，经本地 coturn
（192.168.1.151:3478, test/123）建立通话，断言 selected=relay。

用法：python3 turn_local_test.py
"""

import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time

BIN = "/home/light/webrtc/checkout/src/out/webrtc_client_x64/webrtc_engine_main"
SIGNAL_BIN = "/home/light/webrtc/checkout/src/apps/signal_server_26_11_3/build/signaling_server"
SIGNAL_PORT = 9890
TURN_URI = "turn:192.168.1.151:3478"
TURN_USER = "test"
TURN_PASS = "123"


class Daemon:
    def __init__(self, role, sock):
        self.role = role
        self.sock = sock
        self.q = queue.Queue()
        self.proc = None
        self.rfile = None
        self.wfile = None

    def start(self):
        try:
            os.unlink(self.sock)
        except OSError:
            pass
        self.proc = subprocess.Popen(
            [BIN, f"--socket_path={self.sock}",
             "--ice_transports=relay", f"--turn={TURN_URI}",
             f"--turn_user={TURN_USER}", f"--turn_pass={TURN_PASS}",
             f"--server=127.0.0.1", f"--port={SIGNAL_PORT}",
             "--log_dir=/tmp/wc_turn_logs"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def connect(self):
        for _ in range(40):
            if os.path.exists(self.sock):
                break
            time.sleep(0.25)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.sock)
        f = s.makefile("rw", buffering=1)
        self.rfile = f
        self.wfile = f
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            for line in self.rfile:
                line = line.strip()
                if line:
                    try:
                        self.q.put(json.loads(line))
                    except json.JSONDecodeError:
                        pass
        except (ValueError, OSError):
            pass

    def send(self, line):
        self.wfile.write(line + "\n")
        self.wfile.flush()

    def wait(self, event, pred=None, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                ev = self.q.get(timeout=0.3)
            except queue.Empty:
                continue
            if ev.get("event") == event and (pred is None or pred(ev)):
                return ev
        return None


def wait_connected(o, a, timeout=15):
    deadline = time.time() + timeout
    ok = [False, False]
    while time.time() < deadline:
        if not ok[0]:
            if o.wait("peer_connection.state",
                      lambda e: e.get("state") == "connected", 0.3):
                ok[0] = True
        if not ok[1]:
            if a.wait("peer_connection.state",
                      lambda e: e.get("state") == "connected", 0.3):
                ok[1] = True
        if all(ok):
            return True
    return False


def main():
    sig = subprocess.Popen([SIGNAL_BIN, f"--port={SIGNAL_PORT}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)

    offerer = Daemon("offerer", "/tmp/wc_turn_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_turn_a.sock")
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        offerer.send("register offerer")
        answerer.send("register answerer")

        if not offerer.wait("signal_client.signed_in", timeout=10):
            print("FAIL: offerer not signed in")
            return 1
        if not answerer.wait("signal_client.signed_in", timeout=10):
            print("FAIL: answerer not signed in")
            return 1

        # roster 事件有历史广播（注册时只有自己那条），循环查直到出现 answerer
        aid = 0
        deadline = time.time() + 8
        while time.time() < deadline and not aid:
            offerer.send("get_roster")
            ev = offerer.wait("signal_client.roster", timeout=1.0)
            for p in (ev or {}).get("peers", []):
                if p.get("name") == "answerer":
                    aid = p.get("id", 0)
                    break
        if not aid:
            print("FAIL: answerer not in roster")
            return 1

        offerer.send(f"call {aid}")
        if not wait_connected(offerer, answerer):
            print("FAIL: not connected")
            return 1

        offerer.send("get_stats")
        ev = offerer.wait("stats", timeout=5)
        ctype = (ev or {}).get("candidate_pair", {}).get(
            "local_candidate", {}).get("candidate_type", "")
        cands = (ev or {}).get("local_candidates", [])
        print(f"[offerer] connected=yes selected={ctype}")
        print("local_candidates:",
              " ".join((c.get("candidate_type", "") +
                        ("*" if c.get("selected") else "")) for c in cands))

        if ctype == "relay":
            print("TURN relay ok: selected=relay")
            return 0
        print(f"FAIL: selected={ctype}, expected relay")
        return 1
    finally:
        offerer.send("quit")
        answerer.send("quit")
        time.sleep(0.5)
        sig.terminate()


if __name__ == "__main__":
    sys.exit(main())
