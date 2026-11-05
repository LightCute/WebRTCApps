#!/usr/bin/env python3
"""登录一次后，用同一个 id 重复 20 次：通话 → 挂断 → 再通话（走 unix_socket）。

后端 → 上层的事件为 JSON（每行一个对象），命令保持纯文本。
"""
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.normpath(
    os.path.join(HERE, "../../peerconnection/signal_server_26_11_3/build/signaling_server"))
ENGINE = os.path.normpath(os.path.join(HERE, "../../../out/webrtc_client_x64/webrtc_engine_main"))
PORT = 9895
ROUNDS = 20


class Client:
    def __init__(self, sock_path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(sock_path)
        self.sock.settimeout(0.1)
        self.buf = b""

    def send(self, cmd):
        self.sock.sendall((cmd + "\n").encode())

    def _read(self):
        try:
            data = self.sock.recv(4096)
            if data:
                self.buf += data
                return True
        except socket.timeout:
            pass
        return False

    def next_line(self):
        while b"\n" not in self.buf:
            if not self._read():
                return None
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace")

    def wait_for_event(self, event, field=None, value=None, timeout=6.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.next_line()
            if line is None:
                time.sleep(0.01)
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if obj.get("event") != event:
                continue
            if field is not None and obj.get(field) != value:
                continue
            return obj
        return None

    def close(self):
        self.sock.close()


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    for p in ("/tmp/wt_mc_a.sock", "/tmp/wt_mc_b.sock"):
        if os.path.exists(p):
            os.unlink(p)

    server = subprocess.Popen(
        [SERVER, "--port", str(PORT), "--ping_interval", "2"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_a = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_mc_a.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_b = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_mc_b.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    a = Client("/tmp/wt_mc_a.sock")
    b = Client("/tmp/wt_mc_b.sock")

    try:
        # 登录一次（之后不再 register）
        a.send("register offerer")
        if a.wait_for_event("signal_client.signed_in") is None:
            fail("offerer 未 signed_in")
        b.send("register answerer")
        if b.wait_for_event("signal_client.signed_in") is None:
            fail("answerer 未 signed_in")

        for rnd in range(ROUNDS):
            # 通话
            a.send("call 2")
            if a.wait_for_event("peer_connection.state", "state", "connected",
                                timeout=15) is None:
                fail(f"round {rnd}: offerer 未 connected")
            if b.wait_for_event("peer_connection.state", "state", "connected",
                                timeout=15) is None:
                fail(f"round {rnd}: answerer 未 connected")
            if a.wait_for_event("data_channel.state", "state", "open",
                                timeout=10) is None:
                fail(f"round {rnd}: offerer DataChannel 未 open")
            if b.wait_for_event("data_channel.state", "state", "open",
                                timeout=10) is None:
                fail(f"round {rnd}: answerer DataChannel 未 open")

            # 消息（每轮消息不同，零填充避免子串误匹配）
            msg = f"round-{rnd:02d}"
            a.send(f"data_channel_msg {msg}")
            if b.wait_for_event("data_channel_msg", "text", msg, timeout=5) \
                    is None:
                fail(f"round {rnd}: answerer 未收到消息 {msg}")

            # 挂断
            a.send("leave")
            if a.wait_for_event("peer_connection.state", "state", "closed",
                                timeout=6) is None:
                fail(f"round {rnd}: offerer 未清理")
            if b.wait_for_event("peer_connection.state", "state", "closed",
                                timeout=6) is None:
                fail(f"round {rnd}: answerer 未清理")

            print(f"round {rnd} ok")

        a.send("quit")
        b.send("quit")
        time.sleep(1)
        if eng_a.poll() is None or eng_b.poll() is None:
            fail("quit 后引擎未退出")

        print(f"ipc multicall ok: {ROUNDS} rounds (login once)")
    finally:
        a.close()
        b.close()
        for p in (eng_a, eng_b, server):
            if p.poll() is None:
                p.terminate()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
