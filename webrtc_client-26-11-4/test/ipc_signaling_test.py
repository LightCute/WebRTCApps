#!/usr/bin/env python3
"""信令控制 + 信令转发（无需通话）测试。

覆盖：register 显式 ip/port、切换服务器、signal_disconnect、断开重连、
peer_connection_status（空闲）、signal_msg 转发、离线对端报错。
"""
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.normpath(
    os.path.join(HERE, "../../signal_server_26_11_3/build/signaling_server"))
ENGINE = os.path.normpath(os.path.join(HERE, "../../../out/webrtc_client_x64/webrtc_engine_main"))
PORT_A = 9893
PORT_B = 9892


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
    for p in ("/tmp/wt_sig_a.sock", "/tmp/wt_sig_b.sock"):
        if os.path.exists(p):
            os.unlink(p)

    srv_a = subprocess.Popen(
        [SERVER, "--port", str(PORT_A), "--ping_interval", "2"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    srv_b = subprocess.Popen(
        [SERVER, "--port", str(PORT_B), "--ping_interval", "2"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_a = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_sig_a.sock", "--server", "127.0.0.1",
         "--port", str(PORT_A), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_b = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_sig_b.sock", "--server", "127.0.0.1",
         "--port", str(PORT_A), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    a = Client("/tmp/wt_sig_a.sock")
    b = Client("/tmp/wt_sig_b.sock")

    try:
        # T2: register 显式 ip/port
        a.send(f"register offerer 127.0.0.1 {PORT_A}")
        ev = a.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id", 0) <= 0:
            fail("显式 ip/port 注册失败")
        a_id = ev["id"]

        b.send(f"register answerer 127.0.0.1 {PORT_A}")
        ev = b.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id", 0) <= 0:
            fail("b 注册失败")
        b_id = ev["id"]

        # T6: 空闲状态
        a.send("peer_connection_status")
        ev = a.wait_for_event("peer_connection_status")
        if ev is None or ev.get("signed_in") is not True \
                or ev.get("connected") is not False:
            fail("空闲 peer_connection_status 错误")

        # T9: 信令转发（无通话）
        a.send(f"signal_msg {b_id} hello-via-signal")
        ev = b.wait_for_event("signal_client.message", "text", "hello-via-signal")
        if ev is None or ev.get("from") != a_id:
            fail("signal_msg 转发失败")

        # T10: 离线对端报错
        a.send("signal_msg 999 offline-test")
        if a.wait_for_event("signal_client.error", "code", "peer_offline") is None:
            fail("signal_msg 离线对端未报错")

        # T3: 切换服务器
        b.send(f"register answerer 127.0.0.1 {PORT_B}")
        if b.wait_for_event("signal_client.signed_in") is None:
            fail("切换服务器失败")

        # T5: signal_disconnect
        b.send("signal_disconnect")
        if b.wait_for_event("signal_client.disconnected") is None:
            fail("signal_disconnect 未断开")
        b.send("peer_connection_status")
        ev = b.wait_for_event("peer_connection_status")
        if ev is None or ev.get("signed_in") is not False:
            fail("signal_disconnect 后状态未清理")

        # T4: 断开重连
        b.send(f"register answerer 127.0.0.1 {PORT_B}")
        ev = b.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id", 0) <= 0:
            fail("断开重连失败")

        print("ipc signaling ok: register(ip/port)/switch/disconnect/"
              "reconnect/status/signal_msg")
    finally:
        a.close()
        b.close()
        for p in (eng_a, eng_b, srv_a, srv_b):
            if p.poll() is None:
                p.terminate()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
