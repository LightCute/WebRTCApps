#!/usr/bin/env python3
"""采集能力查询 + 采集档位（分辨率/帧率）切换测试。

拓扑：两个本地 daemon + 云端信令。offerer 用真实摄像头，answerer 用合成设备。
验证：
1. 能力查询返回 格式→尺寸→帧率（含 MJPG/YUYV）；
2. 空闲设置 720p@30 → 通话后远端正收到 1280x720 画面；
3. 通话中切到 640x480@30（不重协商），画面变 640x480；
4. 不支持的档位 → unsupported_profile；
5. video_params 回显采集档位。
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
LOGDIR_O = "/tmp/wc_prof_o"
LOGDIR_A = "/tmp/wc_prof_a"


class Daemon:
    def __init__(self, role, sock, log_dir):
        self.role = role
        self.sock = sock
        self.log_dir = log_dir
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
             f"--log_dir={self.log_dir}"],
            stdout=subprocess.DEVNULL,
            stderr=open(f"/tmp/wc_prof_stderr_{self.role}.log", "w"))

    def connect(self):
        for _ in range(60):
            if os.path.exists(self.sock):
                break
            time.sleep(0.2)
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


def read_logs(log_dir):
    text = ""
    if not os.path.isdir(log_dir):
        return text
    for f in sorted(os.listdir(log_dir)):
        if f.endswith(".log"):
            try:
                with open(os.path.join(log_dir, f), errors="replace") as fh:
                    text += fh.read()
            except OSError:
                pass
    return text


def wait_remote_resolution(log_dir, expected, timeout=20.0):
    """等待远端帧计数日志出现指定分辨率（只看 Remote video frame 行）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in read_logs(log_dir).splitlines():
            if "Remote video frame" in line and expected in line.split("Remote video frame")[-1]:
                return True
        time.sleep(0.3)
    return False


def main():
    for d in (LOGDIR_O, LOGDIR_A):
        subprocess.run(["rm", "-rf", d])
        os.makedirs(d, exist_ok=True)
    offerer = Daemon("offerer", "/tmp/wc_prof_o.sock", LOGDIR_O)
    answerer = Daemon("answerer", "/tmp/wc_prof_a.sock", LOGDIR_A)
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()

        # T0: 设备 + 能力
        offerer.send("get_video_devices")
        ev = offerer.wait("video_devices", timeout=10)
        if not ev:
            fail("T0 未收到 video_devices")
        real = [d for d in ev["devices"] if not d.get("synthetic")]
        if not real:
            fail("T0 需要至少一个真实摄像头")
        cam = real[0]["id"]
        offerer.send(f"get_video_device_capabilities {cam}")
        caps = offerer.wait("video_device_capabilities", timeout=10)
        if not caps or not caps.get("formats"):
            fail(f"T0 能力查询为空: {caps}")
        fmts = [f["pixel_format"] for f in caps["formats"]]
        print(f"T0 {cam} formats={fmts}")
        if "MJPG" not in fmts:
            fail("T0 能力应包含 MJPG")
        mjpg_720 = None
        for f in caps["formats"]:
            if f["pixel_format"] == "MJPG":
                for s in f["sizes"]:
                    if s["width"] == 1280 and s["height"] == 720:
                        mjpg_720 = s["fps"]
        if not mjpg_720 or 30 not in mjpg_720:
            fail(f"T0 MJPG 720p 应支持 30fps，got {mjpg_720}")
        print(f"T0 MJPG 720p fps={mjpg_720}")

        # T0b: selectable 标记（HD 生效 MJPG、SD 生效 YUYV）
        def selectable(fmt_name, w, h):
            for f in caps["formats"]:
                if f["pixel_format"] != fmt_name:
                    continue
                for s in f["sizes"]:
                    if s["width"] == w and s["height"] == h:
                        return s.get("selectable")
            return None

        checks = [
            ("MJPG", 1280, 720, True),
            ("YUYV", 1280, 720, False),
            ("YUYV", 640, 480, True),
            ("MJPG", 640, 480, False),
        ]
        for fmt_name, w, h, expect in checks:
            got = selectable(fmt_name, w, h)
            if got is not None and got != expect:
                fail(f"T0b {fmt_name} {w}x{h} selectable 应为 {expect}，got {got}")
        print("T0b selectable 标记 OK（HD→MJPG，SD→YUYV）")

        # T1: 空闲设置 720p@30
        offerer.send("set_video_profile 1280 720 30")
        ev = offerer.wait("video_profile", timeout=10)
        if not ev or ev.get("result") != "success":
            fail(f"T1 空闲设置 720p@30 失败: {ev}")
        print("T1 空闲设置 1280x720@30 OK")

        # T2: 注册 + 通话 + 开视频 → 远端收到 1280x720
        answerer.send("set_video_device synthetic")
        answerer.wait("video_device", timeout=5)
        suffix = str(int(time.time()) % 100000)
        oname, aname = f"prof_o{suffix}", f"prof_a{suffix}"
        offerer.send(f"register {oname} {SERVER} {PORT}")
        answerer.send(f"register {aname} {SERVER} {PORT}")
        if not offerer.wait("signal_client.signed_in", timeout=10):
            fail("offerer not signed in")
        if not answerer.wait("signal_client.signed_in", timeout=10):
            fail("answerer not signed in")
        time.sleep(0.5)
        aid = peer_id_of(offerer, aname)
        if aid <= 0:
            fail("answerer not in roster")
        offerer.send(f"call {aid}")
        if not offerer.wait("peer_connection.state",
                            lambda e: e.get("state") == "connected", 20):
            fail("T2 call not connected")
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=20):
            fail("T2 answerer 未收到远端视频轨")
        if not wait_remote_resolution(LOGDIR_A, "1280x720"):
            fail("T2 远端未收到 1280x720 画面")
        print("T2 720p 采集传输 OK")

        # T3: 通话中切到 640x480@30（不重协商）
        before = read_logs(LOGDIR_O).count("SendReoffer")
        offerer.send("get_video_status")
        ev = offerer.wait("video_params", timeout=5)
        if ev and (ev.get("capture_width") != 1280 or ev.get("capture_height") != 720):
            fail(f"T3 video_params 采集档位回显错误: {ev}")
        offerer.send("set_video_profile 640 480 30")
        ev = offerer.wait("video_profile", timeout=20)
        if not ev or ev.get("result") != "success":
            fail(f"T3 通话中切 640x480 失败: {ev}")
        if not wait_remote_resolution(LOGDIR_A, "640x480"):
            fail("T3 切换后远端未收到 640x480")
        after = read_logs(LOGDIR_O).count("SendReoffer")
        if after != before:
            fail(f"T3 切采集档位不应触发重协商（{before}->{after}）")
        print("T3 通话中切到 640x480@30 OK（无重协商）")

        # T4: 不支持的档位
        offerer.send("set_video_profile 640 480 60")
        ev = offerer.wait("ipc.error", timeout=10)
        if not ev or ev.get("code") != "unsupported_profile":
            fail(f"T4 期望 unsupported_profile，got {ev}")
        print("T4 不支持档位 -> unsupported_profile OK")

        # T5: video_params 回显采集档位
        offerer.send("get_video_status")
        ev = offerer.wait("video_params", timeout=5)
        if not ev:
            fail("T5 未收到 video_params")
        if ev.get("capture_width") != 640 or ev.get("capture_height") != 480 \
                or ev.get("capture_fps") != 30:
            fail(f"T5 采集档位回显错误: {ev}")
        # 设备回显 + 生效格式（640x480 走 SD 偏好 → YUYV）
        if ev.get("device_id") != cam:
            fail(f"T5 device_id 回显错误: {ev.get('device_id')} != {cam}")
        if not ev.get("device_name"):
            fail("T5 device_name 为空")
        if ev.get("capture_format") != "YUYV":
            fail(f"T5 capture_format 应为 YUYV，got {ev.get('capture_format')}")
        if ev.get("enabled") is not True or ev.get("in_call") is not True:
            fail(f"T5 enabled/in_call 回显错误: {ev}")
        print(f"T5 video_params: device={ev['device_name']} "
              f"{ev['capture_width']}x{ev['capture_height']}@{ev['capture_fps']} "
              f"format={ev['capture_format']} enabled={ev['enabled']} OK")

        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("video profile ok")
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
