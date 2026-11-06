#!/usr/bin/env python3
"""通过 unix_socket 发命令、断言引擎状态（驱动真实 webrtc_engine_main 二进制）。

流程：起信令服务器 + 两个引擎（offerer/answerer），各连各自的 unix socket，
register → call → 等 connected → msg → 等对端收到 → leave → 等清理 → quit。
后端 → 上层的事件均为 JSON（每行一个对象），命令保持纯文本。
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
PORT = 9896


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
        """读行直到出现 event 匹配（可再按 field==value 过滤）的 JSON 对象。"""
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
    for p in ("/tmp/wt_a.sock", "/tmp/wt_b.sock"):
        if os.path.exists(p):
            os.unlink(p)

    server = subprocess.Popen(
        [SERVER, "--port", str(PORT), "--ping_interval", "2"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_a = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_a.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    eng_b = subprocess.Popen(
        [ENGINE, "--socket_path", "/tmp/wt_b.sock", "--server", "127.0.0.1",
         "--port", str(PORT), "--log_dir", "/tmp/wt_logs"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    a = Client("/tmp/wt_a.sock")
    b = Client("/tmp/wt_b.sock")

    try:
        # 0. 连接成功：握手 + 命令清单
        ev = a.wait_for_event("ipc.connected")
        if ev is None:
            fail("未收到 ipc.connected 握手")
        cmds = ev.get("commands", [])
        for c in ("register <name> [ip] [port]", "call <peer_id>",
                  "data_channel_msg <text>", "signal_msg <peer_id> <text>",
                  "leave", "get_roster", "peer_connection_status",
                  "signal_disconnect", "get_stats", "quit"):
            if c not in cmds:
                fail(f"命令清单缺少 {c}")

        # 1. 注册（A 先 -> id 1，B 后 -> id 2）
        a.send("register offerer")
        ev = a.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id") != 1:
            fail("offerer 未 signed_in id=1")
        b.send("register answerer")
        ev = b.wait_for_event("signal_client.signed_in")
        if ev is None or ev.get("id") != 2:
            fail("answerer 未 signed_in id=2")

        # 2. get_roster + status 断言状态（roster 带在线用户状态）
        a.send("get_roster")
        roster = None
        deadline = time.time() + 6
        while time.time() < deadline:
            ev = a.wait_for_event("signal_client.roster", timeout=1.0)
            if ev and len(ev.get("peers", [])) == 2:
                roster = ev
                break
        if roster is None:
            fail("get_roster 未返回两人 roster")
        by_id = {p["id"]: p for p in roster["peers"]}
        if by_id[1].get("name") != "offerer" or by_id[1].get("state") != "idle":
            fail("roster 中 offerer 状态缺失/错误")
        if by_id[2].get("name") != "answerer" or by_id[2].get("state") != "idle":
            fail("roster 中 answerer 状态缺失/错误")

        a.send("peer_connection_status")
        ev = a.wait_for_event("peer_connection_status")
        if ev is None or ev.get("signed_in") is not True:
            fail("status 显示未 signed_in")

        # 3. A 呼叫 B（B id=2）
        a.send("call 2")
        if a.wait_for_event("peer_connection.state", "state", "connected",
                            timeout=15) is None:
            fail("offerer 未 connected")
        if b.wait_for_event("peer_connection.state", "state", "connected",
                            timeout=15) is None:
            fail("answerer 未 connected")
        if b.wait_for_event("data_channel.state", "state", "open",
                            timeout=10) is None:
            fail("answerer DataChannel 未 open")

        # 3.5 get_stats：运行时状态快照（DataChannel + PC GetStats）
        a.send("get_stats")
        ev = a.wait_for_event("stats")
        if ev is None:
            fail("get_stats 无响应")
        if ev.get("data_channel", {}).get("state") != "open" \
                or ev.get("data_channel", {}).get("label") != "chat":
            fail("stats.data_channel 状态错误")
        if not ev.get("candidate_pair"):
            fail("stats 缺少 candidate_pair")

        # 4. 消息送达（A -> B）
        a.send("data_channel_msg hello-from-ipc-test")
        if b.wait_for_event("data_channel_msg", "text", "hello-from-ipc-test") \
                is None:
            fail("answerer 未收到消息")

        # 5. leave -> 双方清理
        a.send("leave")
        if a.wait_for_event("peer_connection.state", "state", "closed",
                            timeout=6.0) is None:
            fail("offerer 未清理(closed)")
        if b.wait_for_event("peer_connection.state", "state", "closed",
                            timeout=6.0) is None:
            fail("answerer 未清理(closed)")

        # 6. quit 正常退出
        a.send("quit")
        b.send("quit")
        time.sleep(1)
        if eng_a.poll() is None or eng_b.poll() is None:
            fail("quit 后引擎未退出")

        print("ipc integration ok: register/call/msg/leave/quit via unix_socket (json)")
    finally:
        a.close()
        b.close()
        for p in (eng_a, eng_b, server):
            if p.poll() is None:
                p.terminate()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
