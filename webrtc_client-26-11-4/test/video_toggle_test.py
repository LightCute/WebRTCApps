#!/usr/bin/env python3
"""通话中视频轨开关测试（默认关视频）。

两个本地 daemon + 云端信令 120.79.210.6:8888。
验证：默认不视频 → call → 通话中 offerer/answerer 各自 video_on/off，
双向都能开/关/再开，事件 remote_video.started/stopped、video.state 正确。
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
ENV = "/home/light/webrtc/checkout/src/apps/webrtc_client-26-11-4/.env"
SERVER = "120.79.210.6"
PORT = 8888


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
            [BIN, f"--socket_path={self.sock}", f"--env_file={ENV}"],
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
        self.q = queue.Queue()
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


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def peer_id_of(d, name, timeout=8.0):
    """轮询 get_roster 直到 roster 里出现 name（避免拿到注册时的陈旧 roster）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        d.send("get_roster")
        ev = d.wait("signal_client.roster", timeout=1.0)
        if ev:
            for p in ev.get("peers", []):
                if p.get("name") == name:
                    return p.get("id", 0)
        time.sleep(0.2)
    return 0


def main():
    offerer = Daemon("offerer", "/tmp/wc_toggle_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_toggle_a.sock")
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        offerer.send(f"register offerer {SERVER} {PORT}")
        answerer.send(f"register answerer {SERVER} {PORT}")
        if not offerer.wait("signal_client.signed_in", timeout=10):
            fail("offerer not signed in")
        if not answerer.wait("signal_client.signed_in", timeout=10):
            fail("answerer not signed in")
        time.sleep(0.5)

        # 默认不视频，call
        aid = peer_id_of(offerer, "answerer")
        if aid <= 0:
            fail("answerer not in roster")
        offerer.send(f"call {aid}")
        if not offerer.wait("peer_connection.state",
                            lambda e: e.get("state") == "connected", 15):
            fail("call not connected")
        print("T0 默认无视频 call connected")

        # T1: offerer video_on → answerer 收到远端视频轨
        offerer.send("video_on")
        if not offerer.wait("video.state", lambda e: e.get("enabled") is True, 5):
            fail("T1 offerer video.state 未置 true")
        if not answerer.wait("remote_video.started", timeout=10):
            fail("T1 answerer 未收到 offerer 的视频轨")
        print("T1 offerer video_on OK")

        # T2: answerer video_on → offerer 收到远端视频轨
        answerer.send("video_on")
        if not answerer.wait("video.state", lambda e: e.get("enabled") is True, 5):
            fail("T2 answerer video.state 未置 true")
        if not offerer.wait("remote_video.started", timeout=10):
            fail("T2 offerer 未收到 answerer 的视频轨")
        print("T2 answerer video_on OK")

        # T3: offerer video_off → answerer 远端视频轨移除
        offerer.send("video_off")
        if not offerer.wait("video.state", lambda e: e.get("enabled") is False, 5):
            fail("T3 offerer video.state 未置 false")
        if not answerer.wait("remote_video.stopped", timeout=10):
            fail("T3 answerer 未收到 remote_video.stopped")
        print("T3 offerer video_off OK")

        # T4: answerer video_off → offerer 远端视频轨移除
        answerer.send("video_off")
        if not answerer.wait("video.state", lambda e: e.get("enabled") is False, 5):
            fail("T4 answerer video.state 未置 false")
        if not offerer.wait("remote_video.stopped", timeout=10):
            fail("T4 offerer 未收到 remote_video.stopped")
        print("T4 answerer video_off OK")

        # T5: 重新打开（关→开），offerer video_on 再开
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=10):
            fail("T5 重开失败：answerer 未再收到 offerer 的视频轨")
        print("T5 offerer 重新 video_on OK")

        # T6: answerer video_on 再开
        answerer.send("video_on")
        if not offerer.wait("remote_video.started", timeout=10):
            fail("T6 重开失败：offerer 未再收到 answerer 的视频轨")
        print("T6 answerer 重新 video_on OK")

        # 挂断
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("video toggle ok")
    finally:
        for d in (offerer, answerer):
            try:
                d.send("quit")
            except Exception:
                pass
        time.sleep(0.5)
        for d in (offerer, answerer):
            try:
                d.proc.terminate()
            except Exception:
                pass


if __name__ == "__main__":
    main()
