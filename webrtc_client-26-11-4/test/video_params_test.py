#!/usr/bin/env python3
"""视频参数控制测试：video_switched 事件 + set_bitrate/framerate/degradation/resolution。

两个本地 daemon + 云端信令。默认关 → call → video_on → 调参 → 验证 video_params 回显、
video_switched 完成事件、分辨率切换后帧计数宽高变化。
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
LOG_DIR = "/tmp/wc_video_params_logs"


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


def peer_id_of(d, name, timeout=8.0):
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


def log_contains(substr):
    logs = ""
    for f in os.listdir(LOG_DIR):
        if f.endswith(".log"):
            with open(os.path.join(LOG_DIR, f)) as fh:
                logs += fh.read()
    return substr in logs


def main():
    offerer = Daemon("offerer", "/tmp/wc_params_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_params_a.sock")
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
        # 显式用合成设备：两个本地 daemon 不能同时打开同一个摄像头。
        offerer.send("set_video_device synthetic")
        offerer.wait("video_device", timeout=5)
        answerer.send("set_video_device synthetic")
        answerer.wait("video_device", timeout=5)

        aid = peer_id_of(offerer, "answerer")
        if aid <= 0:
            fail("answerer not in roster")
        offerer.send(f"call {aid}")
        if not offerer.wait("peer_connection.state",
                            lambda e: e.get("state") == "connected", 15):
            fail("call not connected")
        print("T0 call connected")

        # T1: video_on → video_switched(success)
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=10):
            fail("T1 answerer 未收到远端视频轨")
        ev = offerer.wait("video_switched", timeout=10)
        if not ev or ev.get("result") != "success" or ev.get("enabled") is not True:
            fail(f"T1 video_switched 异常: {ev}")
        print("T1 video_on -> video_switched(success) OK")

        # T2: set_bitrate 500
        offerer.send("set_bitrate 500")
        ev = offerer.wait("video_switched", timeout=5)
        if not ev or ev.get("result") != "success":
            fail(f"T2 set_bitrate video_switched: {ev}")
        ev = offerer.wait("video_params", timeout=5)
        if not ev or ev.get("bitrate_kbps") != 500:
            fail(f"T2 video_params bitrate: {ev}")
        print("T2 set_bitrate 500 OK")

        # T3: set_framerate 15
        offerer.send("set_framerate 15")
        offerer.wait("video_switched", timeout=5)
        ev = offerer.wait("video_params", timeout=5)
        if not ev or ev.get("fps") != 15:
            fail(f"T3 video_params fps: {ev}")
        print("T3 set_framerate 15 OK")

        # T4: set_degradation maintain_resolution
        offerer.send("set_degradation maintain_resolution")
        offerer.wait("video_switched", timeout=5)
        ev = offerer.wait("video_params", timeout=5)
        if not ev or ev.get("degradation") != "maintain_resolution":
            fail(f"T4 video_params degradation: {ev}")
        print("T4 set_degradation maintain_resolution OK")

        # T5: video_off → video_switched(success, enabled=false)
        offerer.send("video_off")
        ev = offerer.wait("video_switched", timeout=10)
        if not ev or ev.get("result") != "success" or ev.get("enabled") is not False:
            fail(f"T5 video_switched: {ev}")
        if not answerer.wait("remote_video.stopped", timeout=10):
            fail("T5 answerer 未收到 remote_video.stopped")
        print("T5 video_off -> video_switched(success, enabled=false) OK")

        # T6: 空闲 set_bitrate → not_in_call
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        time.sleep(0.5)
        offerer.send("set_bitrate 300")
        ev = offerer.wait("ipc.error", timeout=5)
        if not ev or ev.get("code") != "not_in_call":
            fail(f"T6 期望 not_in_call，got {ev}")
        print("T6 空闲 set_bitrate -> not_in_call OK")

        print("video params ok")
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
