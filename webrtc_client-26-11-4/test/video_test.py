#!/usr/bin/env python3
"""视频传输冒烟测试：合成测试图（VP8）+ start_video/stop_video。

两个本地 daemon + 云端信令 120.79.210.6:8888。
验证：start_video/stop_video 命令、call 后双方收到远端视频轨（remote_video.started）、
远端帧计数 sink 有帧流入（日志 "Remote video frame"）。
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
LOG_DIR = "/tmp/wc_video_logs"


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
            [BIN, f"--socket_path={self.sock}", f"--env_file={ENV}",
             f"--log_dir={LOG_DIR}"],
            stdout=subprocess.DEVNULL,
            stderr=open(f"/tmp/wc_stderr_{self.role}.log", "w"))

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


def main():
    offerer = Daemon("offerer", "/tmp/wc_video_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_video_a.sock")
    try:
        os.makedirs(LOG_DIR, exist_ok=True)
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

        # T1: start_video 事件
        offerer.send("start_video")
        ev = offerer.wait("video.state")
        print("T1 offerer start_video:", ev.get("enabled") if ev else None)
        answerer.send("start_video")
        ev = answerer.wait("video.state")
        print("T1 answerer start_video:", ev.get("enabled") if ev else None)

        # T2: call 后双方收到远端视频轨（OnAddTrack 在 SetRemoteDescription 时触发，早于 connected）
        offerer.send("get_roster")
        ev = offerer.wait("signal_client.roster")
        aid = next(p["id"] for p in ev.get("peers", []) if p["name"] == "answerer")
        offerer.send(f"call {aid}")
        # remote_video.started 先于 connected 触发，须先等它，否则会被 connected 的 wait 丢弃
        if not offerer.wait("remote_video.started", timeout=15):
            fail("offerer 未收到远端视频轨")
        if not answerer.wait("remote_video.started", timeout=15):
            fail("answerer 未收到远端视频轨")
        if not offerer.wait("peer_connection.state",
                            lambda e: e.get("state") == "connected", 15):
            fail("not connected")
        print("T2 双方均收到远端视频轨 remote_video.started + connected")

        # T3: 等 2 秒，检查日志有 "Remote video frame"
        time.sleep(3)
        logs = ""
        for f in os.listdir(LOG_DIR):
            if f.endswith(".log"):
                with open(os.path.join(LOG_DIR, f)) as fh:
                    logs += fh.read()
        if "Remote video frame #1" not in logs:
            fail("日志未见远端视频帧计数")
        print("T3 远端视频帧计数日志 OK")

        # T4: stop_video 事件
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        offerer.send("stop_video")
        ev = offerer.wait("video.state")
        print("T4 offerer stop_video:", ev.get("enabled") if ev else None)

        print("video test ok")
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
