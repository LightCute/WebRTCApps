#!/usr/bin/env python3
"""视频设备枚举 / 选择 / 通话中切换测试。

拓扑：两个本地 daemon + 云端信令 120.79.210.6:8888。
- offerer 用真实 USB 摄像头，answerer 用合成设备（同一摄像头不能被两个进程同时打开）。
- 验证：枚举、空闲选择、通话中切换（不重协商）、反复切换不崩、切换后开关视频不崩。
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
LOGDIR_O = "/tmp/wc_devlog_o"
LOGDIR_A = "/tmp/wc_devlog_a"


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
            stderr=open(f"/tmp/wc_dev_stderr_{self.role}.log", "w"))

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

    def alive(self):
        return self.proc is not None and self.proc.poll() is None


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


def wait_log(log_dir, needle, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_logs(log_dir):
            return True
        time.sleep(0.3)
    return False


def main():
    for d in (LOGDIR_O, LOGDIR_A):
        subprocess.run(["rm", "-rf", d])
        os.makedirs(d, exist_ok=True)
    offerer = Daemon("offerer", "/tmp/wc_dev_o.sock", LOGDIR_O)
    answerer = Daemon("answerer", "/tmp/wc_dev_a.sock", LOGDIR_A)
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()

        # T0: 枚举
        offerer.send("get_video_devices")
        ev = offerer.wait("video_devices", timeout=10)
        if not ev:
            fail("T0 未收到 video_devices")
        devices = ev.get("devices", [])
        ids = [d["id"] for d in devices]
        real = [d for d in devices if not d.get("synthetic")]
        print(f"T0 devices={ids} current={ev.get('current')}")
        if "synthetic" not in ids or "synthetic_hd" not in ids:
            fail("T0 缺少合成设备")
        if not real:
            fail("T0 未枚举到真实摄像头（请确认摄像头已插入）")
        real_id = real[0]["id"]

        # 注册（唯一名字，避免上一轮残留导致 name_taken）
        suffix = str(int(time.time()) % 100000)
        offerer_name = f"offerer{suffix}"
        answerer_name = f"answerer{suffix}"
        offerer.send(f"register {offerer_name} {SERVER} {PORT}")
        answerer.send(f"register {answerer_name} {SERVER} {PORT}")
        if not offerer.wait("signal_client.signed_in", timeout=10):
            fail("offerer not signed in")
        if not answerer.wait("signal_client.signed_in", timeout=10):
            fail("answerer not signed in")
        time.sleep(0.5)

        # T1: 空闲选择设备
        offerer.send(f"set_video_device {real_id}")
        ev = offerer.wait("video_device", timeout=5)
        print("T1 offerer idle set real ->", ev.get("result") if ev else None)
        if not ev or ev.get("result") != "success":
            fail("T1 空闲选择真实设备失败")
        answerer.send("set_video_device synthetic_hd")
        ev = answerer.wait("video_device", timeout=5)
        if not ev or ev.get("result") != "success":
            fail("T1 空闲选择 synthetic_hd 失败")

        # 建通话
        aid = peer_id_of(offerer, answerer_name)
        if aid <= 0:
            fail("answerer not in roster")
        offerer.send(f"call {aid}")
        if not offerer.wait("peer_connection.state",
                            lambda e: e.get("state") == "connected", 20):
            fail("call not connected")
        print("T1 call connected")

        # T2: 开视频，answerer 应收到 640x480 的真实摄像头画面
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=15):
            fail("T2 answerer 未收到远端视频轨")
        if not wait_log(LOGDIR_A, "640x480", timeout=15):
            fail("T2 answerer 日志未出现 640x480（真实摄像头画面）")
        print("T2 真实摄像头 640x480 传输 OK")

        # T3: 通话中切到 synthetic_hd → 画面变 1280x720，且不重协商
        before = read_logs(LOGDIR_O).count("SendReoffer") + read_logs(LOGDIR_O).count("OnSignalReoffer")
        offerer.send("set_video_device synthetic_hd")
        ev = offerer.wait("video_device", timeout=10)
        print("T3 in-call switch -> synthetic_hd:", ev.get("result") if ev else None)
        if not ev or ev.get("result") != "success":
            fail("T3 通话中切换失败")
        if not wait_log(LOGDIR_A, "1280x720", timeout=15):
            fail("T3 切换后未出现 1280x720 画面")
        after = read_logs(LOGDIR_O).count("SendReoffer") + read_logs(LOGDIR_O).count("OnSignalReoffer")
        if after != before:
            fail(f"T3 切换设备不应触发重协商（reoffer {before}->{after}）")
        print("T3 切换到 synthetic_hd OK（1280x720，无重协商）")

        # T4: 来回切换 10 次不崩
        for i in range(10):
            target = real_id if i % 2 == 0 else "synthetic_hd"
            offerer.send(f"set_video_device {target}")
            ev = offerer.wait("video_device", timeout=15)
            if not ev or ev.get("result") != "success":
                fail(f"T4 第 {i} 次切换失败: {ev}")
            if not offerer.alive():
                fail(f"T4 第 {i} 次切换后进程崩溃")
        print("T4 反复切换 10 次 OK（无崩溃）")

        # T5: 切换后关视频不崩
        offerer.send("video_off")
        ev = offerer.wait("video_switched", timeout=15)
        if not ev or ev.get("result") != "success":
            fail(f"T5 video_off 失败: {ev}")
        if not answerer.wait("remote_video.stopped", timeout=10):
            fail("T5 answerer 未收到 remote_video.stopped")
        if not offerer.alive():
            fail("T5 关视频后进程崩溃（FindSinkPair 场景）")
        print("T5 切换后 video_off OK（无崩溃）")

        # T6: 再开视频
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=15):
            fail("T6 video_on 未恢复")
        print("T6 video_on 恢复 OK")

        # T7: 不存在的设备
        offerer.send("set_video_device nonsense")
        ev = offerer.wait("ipc.error", timeout=5)
        if not ev or ev.get("code") != "device_not_found":
            fail(f"T7 期望 device_not_found，got {ev}")
        print("T7 非法设备 -> device_not_found OK")

        # 收尾
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("video device ok")
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
