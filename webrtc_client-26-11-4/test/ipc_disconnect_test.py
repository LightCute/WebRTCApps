#!/usr/bin/env python3
"""通话中的信令断开清理测试。

覆盖：通话中 peer_connection_status、signal_disconnect 拆 PC/DC、
信令服务器崩溃拆 PC/DC。
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
PORT = 9891


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


def establish_call(a, b, b_id):
    """a 呼叫 b，等双方 PC 连通 + DC 打开。"""
    a.send(f"call {b_id}")
    if a.wait_for_event("peer_connection.state", "state", "connected",
                        timeout=15) is None:
        fail("offerer 未 connected")
    if b.wait_for_event("peer_connection.state", "state", "connected",
                        timeout=15) is None:
        fail("answerer 未 connected")
    if a.wait_for_event("data_channel.state", "state", "open",
                        timeout=10) is None:
        fail("offerer DataChannel 未 open")
    if b.wait_for_event("data_channel.state", "state", "open",
                        timeout=10) is None:
        fail("answerer DataChannel 未 open")


def main():
    for p in ("/tmp/wt_dc_a.sock", "/tmp/wt_dc_b.sock"):
        if os.path.exists(p):
            os.unlink(p)

    srv = subprocess.Popen(
        [SERVER, "--port", str(PORT), "--ping_interval", "2"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_a = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_dc_a.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_b = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_dc_b.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    a = Client("/tmp/wt_dc_a.sock")
    b = Client("/tmp/wt_dc_b.sock")

    try:
        # 注册
        a.send("register offerer")
        ev = a.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id", 0) <= 0:
            fail("offerer 未 signed_in")
        b.send("register answerer")
        ev = b.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id", 0) <= 0:
            fail("answerer 未 signed_in")
        b_id = ev["id"]

        # 通话 1
        establish_call(a, b, b_id)

        # T7: 通话中 status
        a.send("peer_connection_status")
        ev = a.wait_for_event("peer_connection_status")
        if ev is None or ev.get("connected") is not True \
                or ev.get("data_channel_open") is not True \
                or ev.get("peer_id") != b_id:
            fail("通话中 peer_connection_status 错误")

        # T12: signal_disconnect 拆 PC/DC
        a.send("signal_disconnect")
        if a.wait_for_event("signal_client.disconnected") is None:
            fail("signal_disconnect 未 disconnected")
        if a.wait_for_event("peer_connection.state", "state", "closed",
                            timeout=10) is None:
            fail("signal_disconnect 未拆本端 PC")
        if b.wait_for_event("peer_connection.state", "state", "closed",
                            timeout=10) is None:
            fail("signal_disconnect 后对端未清理")

        # 重建通话（a 重连，b 仍在服务器上）
        a.send("register offerer")
        if a.wait_for_event("signal_client.signed_in") is None:
            fail("a 重连失败")
        establish_call(a, b, b_id)

        # T11: 信令服务器崩溃拆 PC/DC
        srv.terminate()
        srv.wait()
        if a.wait_for_event("signal_client.disconnected", timeout=15) is None:
            fail("崩溃后 offerer 未 disconnected")
        if a.wait_for_event("peer_connection.state", "state", "closed",
                            timeout=15) is None:
            fail("崩溃后 offerer 未拆 PC")
        if b.wait_for_event("signal_client.disconnected", timeout=15) is None:
            fail("崩溃后 answerer 未 disconnected")
        if b.wait_for_event("peer_connection.state", "state", "closed",
                            timeout=15) is None:
            fail("崩溃后 answerer 未拆 PC")

        print("ipc disconnect ok: signal_disconnect + server crash teardown")
    finally:
        a.close()
        b.close()
        for p in (eng_a, eng_b, srv):
            if p.poll() is None:
                p.terminate()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
