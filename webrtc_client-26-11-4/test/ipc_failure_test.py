#!/usr/bin/env python3
"""命令失败返回（受理校验）测试。

覆盖：未注册/未通话状态下，各命令应回 ipc.error 并携带对应错误码；
通话中再 call 应回 already_in_call。
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
PORT = 9890
PORT_DEAD = 9889  # 无服务监听，用于测试连接失败


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


def connect_retry(sock_path, timeout=10.0):
    """连接 unix socket，带重试（引擎启动可能慢于固定 sleep）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return Client(sock_path)
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.2)
    raise RuntimeError("engine not ready: " + sock_path)


def wait_for_port(port, timeout=10.0):
    """等待指定端口开始监听（信号服务器启动完成），避免固定 sleep 在负载下不够。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.5)
        if s.connect_ex(("127.0.0.1", port)) == 0:
            s.close()
            return
        s.close()
        time.sleep(0.2)
    raise RuntimeError("port not listening: " + str(port))


def main():
    for p in ("/tmp/wt_fail_a.sock", "/tmp/wt_fail_b.sock", "/tmp/wt_fail_c.sock",
              "/tmp/wt_fail_d.sock"):
        if os.path.exists(p):
            os.unlink(p)

    srv = subprocess.Popen(
        [SERVER, "--port", str(PORT), "--ping_interval", "2"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for_port(PORT)
    eng_a = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_fail_a.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_b = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_fail_b.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_c = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_fail_c.sock", "--server", "127.0.0.1",
         "--port", str(PORT_DEAD), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_d = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_fail_d.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    a = connect_retry("/tmp/wt_fail_a.sock")
    b = connect_retry("/tmp/wt_fail_b.sock")
    c = connect_retry("/tmp/wt_fail_c.sock")
    d = connect_retry("/tmp/wt_fail_d.sock")

    try:
        # ===== 连接不可达：register 应收到 connect 错误 =====
        c.send("register c")
        if c.wait_for_event("signal_client.error", "code", "connect", timeout=8.0) is None:
            fail("连接不可达未报 connect 错误")

        # ===== 未注册：受理校验应即时拒绝 =====
        a.send("data_channel_msg hello")
        if a.wait_for_event("ipc.error", "code", "data_channel_not_open") is None:
            fail("未通话 data_channel_msg 未报 data_channel_not_open")

        a.send("signal_msg 2 hi")
        if a.wait_for_event("ipc.error", "code", "signal_send_failed") is None:
            fail("未注册 signal_msg 未报 signal_send_failed")

        a.send("get_roster")
        if a.wait_for_event("ipc.error", "code", "not_signed_in") is None:
            fail("未注册 get_roster 未报 not_signed_in")

        a.send("leave")
        if a.wait_for_event("ipc.error", "code", "not_signed_in") is None:
            fail("未注册 leave 未报 not_signed_in")

        a.send("call 2")
        if a.wait_for_event("ipc.error", "code", "not_signed_in") is None:
            fail("未注册 call 未报 not_signed_in")

        a.send("signal_disconnect")
        if a.wait_for_event("ipc.error", "code", "not_signed_in") is None:
            fail("未注册 signal_disconnect 未报 not_signed_in")

        # ===== 注册 + 通话，通话中再 call =====
        a.send("register a")
        ev = a.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id", 0) <= 0:
            fail("a 注册失败")
        a_id = ev["id"]
        b.send("register b")
        ev = b.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id", 0) <= 0:
            fail("b 注册失败")
        b_id = ev["id"]

        # 等 a 的 roster 广播刷新（b 刚注册），避免 Layer 1 用过期 peers_ 误判 peer_offline
        time.sleep(0.5)

        # ===== 呼叫自己的 id：应拒绝 self_call =====
        a.send(f"call {a_id}")
        if a.wait_for_event("ipc.error", "code", "self_call") is None:
            fail("呼叫自己的 id 未报 self_call")

        # ===== 呼叫不存在的 id：应拒绝 peer_offline =====
        a.send("call 9999")
        if a.wait_for_event("ipc.error", "code", "peer_offline") is None:
            fail("呼叫不存在的 id 未报 peer_offline")

        # ===== 已登录但不在通话：leave 应拒绝 not_in_call =====
        a.send("leave")
        if a.wait_for_event("ipc.error", "code", "not_in_call") is None:
            fail("不在通话 leave 未报 not_in_call")

        a.send(f"call {b_id}")
        if a.wait_for_event("peer_connection.state", "state", "connected") is None:
            fail("通话未 connected")
        if b.wait_for_event("peer_connection.state", "state", "connected") is None:
            fail("对端未 connected")

        # ===== 呼叫忙线的对端：应拒绝 peer_busy =====
        d.send("register d")
        if d.wait_for_event("signal_client.signed_in") is None:
            fail("d 注册失败")
        d.wait_for_event("signal_client.roster")  # 等 roster 刷新（b 已 busy）
        d.send(f"call {b_id}")
        if d.wait_for_event("ipc.error", "code", "peer_busy") is None:
            fail("呼叫忙线的对端未报 peer_busy")

        a.send(f"call {b_id}")
        if a.wait_for_event("ipc.error", "code", "already_in_call") is None:
            fail("通话中再 call 未报 already_in_call")

        # 清理
        a.send("leave")
        a.wait_for_event("signal_client.call_ended")
        b.wait_for_event("signal_client.call_ended")

        print("ipc failure ok: connect/data_channel_not_open/signal_send_failed/"
              "not_signed_in/not_in_call/self_call/peer_offline/peer_busy/already_in_call")
    finally:
        a.close()
        b.close()
        c.close()
        d.close()
        for p in (eng_a, eng_b, eng_c, eng_d, srv):
            if p.poll() is None:
                p.terminate()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
