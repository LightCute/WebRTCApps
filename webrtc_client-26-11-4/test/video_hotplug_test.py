#!/usr/bin/env python3
"""摄像头热插拔测试（netlink uevent 监听）。

用 sysfs 解绑/重绑 uvcvideo 驱动接口来**真实模拟拔插**（内核会产生真正的 uevent）：
  echo <iface> > /sys/bus/usb/drivers/uvcvideo/unbind
  echo <iface> > /sys/bus/usb/drivers/uvcvideo/bind

需要 sudo。运行方式：
  SUDO_PASS=<密码> python3 video_hotplug_test.py
验证：
1. 通话中拔掉正在使用的摄像头 → 发 video_device_lost，视频通道关闭（对端 remote_video.stopped）；
2. 插回 → 发 video_devices_changed(reason=device_added)，不自动开视频；
3. 再次 video_on → 视频恢复。
"""

import glob
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
LOGDIR_O = "/tmp/wc_hot_o"
LOGDIR_A = "/tmp/wc_hot_a"
UVC_DRIVER = "/sys/bus/usb/drivers/uvcvideo"


class Daemon:
    def __init__(self, role, sock, log_dir):
        self.role = role
        self.sock = sock
        self.log_dir = log_dir
        self.q = queue.Queue()
        self.pending = []
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
            stderr=open(f"/tmp/wc_hot_stderr_{self.role}.log", "w"))

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

    def wait(self, event, pred=None, timeout=8.0):
        # 未匹配的事件保留在 pending 里，避免被后续 wait 吞掉。
        for i, ev in enumerate(self.pending):
            if ev.get("event") == event and (pred is None or pred(ev)):
                return self.pending.pop(i)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                ev = self.q.get(timeout=0.3)
            except queue.Empty:
                continue
            if ev.get("event") == event and (pred is None or pred(ev)):
                return ev
            self.pending.append(ev)
        return None

    def alive(self):
        return self.proc is not None and self.proc.poll() is None


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def sysfs_write(path, value):
    password = os.environ.get("SUDO_PASS", "")
    script = f"echo {value} > {path}"
    if password:
        r = subprocess.run(["sudo", "-S", "sh", "-c", script],
                           input=password + "\n", text=True,
                           capture_output=True)
    else:
        r = subprocess.run(["sudo", "-n", "sh", "-c", script],
                           text=True, capture_output=True)
    if r.returncode != 0:
        print(f"  sysfs 写入失败: {path} <- {value}: {r.stderr.strip()}")
        return False
    return True


def iface_for_camera(bus_info):
    """从 bus_info（usb-0000:00:14.0-11.2）推导 uvcvideo 接口名（1-11.2:1.0）。"""
    port = bus_info.split("-")[-1] if "-" in bus_info else bus_info
    candidates = glob.glob(f"{UVC_DRIVER}/*-{port}:1.0")
    return os.path.basename(candidates[0]) if candidates else None


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
    for f in sorted(os.listdir(log_dir)) if os.path.isdir(log_dir) else []:
        if f.endswith(".log"):
            with open(os.path.join(log_dir, f), errors="replace") as fh:
                text += fh.read()
    return text


def wait_remote_resolution(log_dir, expected, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in read_logs(log_dir).splitlines():
            if "Remote video frame" in line and expected in line.split("Remote video frame")[-1]:
                return True
        time.sleep(0.3)
    return False


def main():
    if not os.path.isdir(UVC_DRIVER):
        fail(f"{UVC_DRIVER} 不存在（需要 uvcvideo 驱动）")
    for d in (LOGDIR_O, LOGDIR_A):
        subprocess.run(["rm", "-rf", d])
        os.makedirs(d, exist_ok=True)

    offerer = Daemon("offerer", "/tmp/wc_hot_o.sock", LOGDIR_O)
    answerer = Daemon("answerer", "/tmp/wc_hot_a.sock", LOGDIR_A)
    iface = None
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()

        # T0: 设备枚举
        offerer.send("get_video_devices")
        ev = offerer.wait("video_devices", timeout=10)
        if not ev:
            fail("T0 未收到 video_devices")
        real = [d for d in ev["devices"] if not d.get("synthetic")]
        if not real:
            fail("T0 需要至少一个真实摄像头")
        cam_id = real[0]["id"]
        bus_info = cam_id.split(":", 1)[1]
        iface = iface_for_camera(bus_info)
        if not iface:
            fail(f"T0 找不到 {cam_id} 对应的 uvcvideo 接口")
        print(f"T0 摄像头={cam_id} 接口={iface}")

        # 建通话 + 开视频
        answerer.send("set_video_device synthetic")
        answerer.wait("video_device", timeout=5)
        offerer.send(f"set_video_device {cam_id}")
        offerer.wait("video_device", timeout=5)
        suffix = str(int(time.time()) % 100000)
        oname, aname = f"hot_o{suffix}", f"hot_a{suffix}"
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
            fail("call not connected")
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=20):
            fail("answerer 未收到远端视频轨")
        if not wait_remote_resolution(LOGDIR_A, "640x480"):
            fail("远端未收到 640x480 画面")
        print("T1 视频通话建立，画面正常")

        # T2: 拔掉摄像头
        if not sysfs_write(f"{UVC_DRIVER}/unbind", iface):
            fail("T2 无法解绑设备（需要 sudo：SUDO_PASS=<密码>）")
        lost = offerer.wait("video_device_lost", timeout=15)
        if not lost:
            fail("T2 未收到 video_device_lost")
        print(f"T2 video_device_lost: {lost.get('id')}")
        sw = offerer.wait("video_switched",
                          lambda e: e.get("enabled") is False, timeout=15)
        if not sw:
            fail("T2 未收到 video_switched(enabled=false)")
        print("T2 视频通道已关闭 video_switched(enabled=false)")
        if not answerer.wait("remote_video.stopped", timeout=15):
            fail("T2 对端未收到 remote_video.stopped")
        print("T2 对端收到 remote_video.stopped")
        changed = offerer.wait("video_devices_changed", timeout=15)
        if not changed or changed.get("reason") != "device_removed":
            fail(f"T2 设备变化事件异常: {changed}")
        if not offerer.alive():
            fail("T2 拔设备后进程崩溃")

        # T3: 插回摄像头
        if not sysfs_write(f"{UVC_DRIVER}/bind", iface):
            fail("T3 无法重新绑定设备")
        changed = offerer.wait(
            "video_devices_changed",
            lambda e: e.get("reason") == "device_added", timeout=20)
        if not changed:
            fail("T3 未收到 device_added 通知")
        ids = [d["id"] for d in changed.get("devices", [])]
        if cam_id not in ids:
            fail(f"T3 插回后设备列表未包含 {cam_id}: {ids}")
        print("T3 插回通知 OK（未自动开视频）")

        # T4: 再次开视频
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=25):
            fail("T4 重新 video_on 失败")
        if not wait_remote_resolution(LOGDIR_A, "640x480", timeout=25):
            fail("T4 恢复后画面异常")
        print("T4 重新开视频 OK")

        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("hotplug test ok")
    finally:
        # 确保设备恢复绑定
        if iface:
            sysfs_write(f"{UVC_DRIVER}/bind", iface)
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
