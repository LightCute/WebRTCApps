#!/usr/bin/env python3
"""P1 媒体压测：本地多摄像头的切换 / 档位遍历 / 占用冲突 / 反复切换。

拓扑：两个本地 daemon（信令走云端 8888，媒体走云端 coturn relay）。
offerer 使用真实摄像头（可切换），answerer 固定 synthetic。

覆盖：
  T0 枚举（>=3 个真实设备）+ 每个设备能力查询
  T1 建通话 + 开视频
  T2 三个摄像头依次切换（画面继续、无重协商）
  T3 每个摄像头遍历 selectable 档位（远端分辨率跟着变）
  T4 占用冲突：answerer 抢 offerer 正在用的摄像头 → failed 且保留原设备
  T5 三摄像头反复切换 5 轮（不崩、画面持续）
  T6 切到合成设备再切回真实摄像头
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
LOGDIR_O = "/tmp/wc_media_o"
LOGDIR_A = "/tmp/wc_media_a"


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
            stderr=open(f"/tmp/wc_media_stderr_{self.role}.log", "w"))

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

    def drain(self, event):
        while True:
            try:
                ev = self.q.get_nowait()
            except queue.Empty:
                break
            self.pending.append(ev)
        self.pending = [e for e in self.pending if e.get("event") != event]

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


def wait_remote_res(log_dir, expected, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in read_logs(log_dir).splitlines():
            if "Remote video frame" in line and expected in line.split("Remote video frame")[-1]:
                return True
        time.sleep(0.3)
    return False


def latest_bytes(d, timeout=10.0):
    st = d.wait("video_stats", timeout=timeout)
    return st.get("bytes_sent", 0) if st else None


def get_devices(d):
    d.send("get_video_devices")
    ev = d.wait("video_devices", timeout=10)
    return ev.get("devices", []) if ev else []


def get_caps(d, dev_id):
    d.send(f"get_video_device_capabilities {dev_id}")
    ev = d.wait("video_device_capabilities", timeout=10)
    return ev.get("formats", []) if ev else []


def sudo_shell(script):
    password = os.environ.get("SUDO_PASS", "")
    if password:
        r = subprocess.run(["sudo", "-S", "sh", "-c", script],
                           input=password + "\n", text=True, capture_output=True)
    else:
        r = subprocess.run(["sudo", "-n", "sh", "-c", script],
                           text=True, capture_output=True)
    return r.returncode == 0, r.stderr.strip()


def node_for_camera(cam_id):
    """cam_id = "v4l2:usb-0000:00:14.0-8" -> /dev/videoN"""
    bus = cam_id.split(":", 1)[-1]
    out = subprocess.run(["v4l2-ctl", "--list-devices"], text=True,
                         capture_output=True).stdout
    in_block = False
    for line in out.splitlines():
        if bus in line:
            in_block = True
            continue
        if in_block:
            stripped = line.strip()
            if stripped.startswith("/dev/video"):
                return stripped
            if stripped and not stripped.startswith("/dev/"):
                in_block = False
    return None


def selectable_profiles(formats):
    """返回 [(w,h,fps)]，只取 selectable=true 的尺寸，fps 取该尺寸最大值。"""
    out = []
    for f in formats:
        for s in f.get("sizes", []):
            if not s.get("selectable"):
                continue
            fps = s.get("fps") or []
            if not fps:
                continue
            out.append((s["width"], s["height"], max(fps)))
    # 去重（同一尺寸可能出现在不同格式里，但只有一个 selectable）
    seen = set()
    uniq = []
    for p in out:
        if p[:2] in seen:
            continue
        seen.add(p[:2])
        uniq.append(p)
    # 按面积排序，限制到 <= 1280x720 且最多 4 个，控制测试时长
    uniq = [p for p in uniq if p[0] * p[1] <= 1280 * 720]
    uniq.sort(key=lambda p: p[0] * p[1])
    if len(uniq) > 4:
        step = len(uniq) / 4.0
        uniq = [uniq[int(i * step)] for i in range(4)]
    return uniq


def main():
    for d in (LOGDIR_O, LOGDIR_A):
        subprocess.run(["rm", "-rf", d])
        os.makedirs(d, exist_ok=True)
    offerer = Daemon("offerer", "/tmp/wc_media_o.sock", LOGDIR_O)
    answerer = Daemon("answerer", "/tmp/wc_media_a.sock", LOGDIR_A)
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()

        # ---- T0: 枚举 + 能力 ----
        devices = get_devices(offerer)
        real = [d for d in devices if not d.get("synthetic")]
        print(f"T0 设备: {[d['id'] for d in devices]}")
        if len(real) < 3:
            fail(f"T0 需要 3 个真实摄像头，实际 {len(real)}")
        cams = [d["id"] for d in real]
        caps_by_cam = {}
        for cam in cams:
            caps = get_caps(offerer, cam)
            if not caps:
                fail(f"T0 {cam} 能力为空")
            caps_by_cam[cam] = caps
            fmts = [f["pixel_format"] for f in caps]
            profs = selectable_profiles(caps)
            print(f"T0 {cam}: formats={fmts} 可选档位={[(w,h,f) for w,h,f in profs]}")

        # ---- T1: 建通话 + 开视频 ----
        answerer.send("set_video_device synthetic")
        answerer.wait("video_device", timeout=5)
        offerer.send(f"set_video_device {cams[0]}")
        ev = offerer.wait("video_device", timeout=10)
        if not ev or ev.get("result") != "success":
            fail(f"T1 选择 {cams[0]} 失败: {ev}")

        suffix = str(int(time.time()) % 100000)
        oname, aname = f"md_o{suffix}", f"md_a{suffix}"
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
            fail("T1 未连通")
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=20):
            fail("T1 answerer 未收到视频")
        offerer.send("set_video_stats on 1000")
        b0 = latest_bytes(offerer)
        print(f"T1 通话 + 视频就绪（bytes={b0}）")

        # ---- T2: 三摄像头依次切换 ----
        print("-- T2 三摄像头切换 --")
        for cam in cams[1:] + [cams[0]]:
            before = latest_bytes(offerer) or 0
            offerer.send(f"set_video_device {cam}")
            ev = offerer.wait("video_device", timeout=20)
            if not ev or ev.get("result") != "success":
                fail(f"T2 切到 {cam} 失败: {ev}")
            time.sleep(2)
            after = latest_bytes(offerer)
            if after is None or after <= before:
                fail(f"T2 切到 {cam} 后画面停止（{before}->{after}）")
            print(f"  [PASS] 切到 {cam}（bytes {before}->{after}）")

        # ---- T3: 每个摄像头遍历档位 ----
        print("-- T3 档位遍历 --")
        current_cam = cams[0]
        for cam in cams:
            if cam != current_cam:
                offerer.send(f"set_video_device {cam}")
                if not offerer.wait("video_device", timeout=20):
                    fail(f"T3 切到 {cam} 失败")
                current_cam = cam
            for (w, h, fps) in selectable_profiles(caps_by_cam[cam]):
                before = latest_bytes(offerer) or 0
                offerer.send(f"set_video_profile {w} {h} {fps}")
                ev = offerer.wait("video_profile", timeout=25)
                if not ev or ev.get("result") != "success":
                    fail(f"T3 {cam} 切档 {w}x{h}@{fps} 失败: {ev}")
                if not wait_remote_res(LOGDIR_A, f"{w}x{h}", timeout=20):
                    fail(f"T3 {cam} {w}x{h} 远端未收到该分辨率画面")
                time.sleep(1)
                after = latest_bytes(offerer)
                if after is None or after <= before:
                    fail(f"T3 {cam} {w}x{h} 切档后画面停止")
                print(f"  [PASS] {cam} -> {w}x{h}@{fps}（远端已收该分辨率）")

        # ---- T4a: 抢占 offerer 正在用的摄像头（结果取决于驱动是否允许多开） ----
        print("-- T4a 抢占在用摄像头 --")
        offerer.send("get_video_status")
        ev = offerer.wait("video_params", timeout=5)
        busy_cam = ev.get("device_id") if ev else None
        if not busy_cam:
            fail("T4a 无法获取 offerer 当前设备")
        offerer_before = latest_bytes(offerer) or 0
        answerer.send(f"set_video_device {busy_cam}")
        ev = answerer.wait("video_device", timeout=20)
        if not ev:
            fail("T4a 未收到 video_device")
        if ev.get("result") == "failed":
            answerer.send("get_video_status")
            ev2 = answerer.wait("video_params", timeout=5)
            if not ev2 or ev2.get("device_id") != "synthetic":
                fail(f"T4a 失败后应保留 synthetic，got {ev2 and ev2.get('device_id')}")
            print("  [PASS] 抢占失败且保留原设备（驱动独占）")
        else:
            # 驱动允许多开：确认 offerer 的推流未受影响，然后切回 synthetic
            time.sleep(2)
            offerer_after = latest_bytes(offerer)
            if offerer_after is None or offerer_after <= offerer_before:
                fail(f"T4a 抢占后 offerer 画面停止（{offerer_before}->{offerer_after}）")
            answerer.send("set_video_device synthetic")
            answerer.wait("video_device", timeout=10)
            print("  [PASS] 驱动允许多开；抢占后 offerer 画面未受影响（已切回 synthetic）")

        # ---- T4b: 确定性失败路径（设备节点不可读，需 SUDO_PASS） ----
        print("-- T4b 不可读设备 --")
        victim = cams[2] if cams[2] != busy_cam else cams[1]
        node = node_for_camera(victim)
        if not node:
            print("  [SKIP] 找不到设备节点")
        elif not os.environ.get("SUDO_PASS"):
            print("  [SKIP] 未提供 SUDO_PASS")
        else:
            ok, err = sudo_shell(f"chmod 000 {node}")
            if not ok:
                print(f"  [SKIP] chmod 失败: {err}")
            else:
                try:
                    answerer.drain("video_params")
                    answerer.send("get_video_status")
                    ev = answerer.wait("video_params", timeout=5)
                    orig = ev.get("device_id") if ev else None
                    print(f"  (T4b 目标设备={victim} 节点={node} 当前={orig})")
                    answerer.send(f"set_video_device {victim}")
                    # 设备仍在枚举里但打不开 → video_device failed；
                    # 设备从枚举里消失 → ipc.error device_not_found。两者都算优雅失败。
                    ev = answerer.wait(
                        "video_device", lambda e: e.get("result") == "failed",
                        timeout=10)
                    if not ev:
                        ev = answerer.wait(
                            "ipc.error",
                            lambda e: e.get("code") == "device_not_found",
                            timeout=5)
                    if not ev:
                        fail("T4b 不可读设备既没有 video_device failed 也没有 device_not_found")
                    answerer.drain("video_params")
                    answerer.send("get_video_status")
                    ev = answerer.wait("video_params", timeout=5)
                    if not ev or ev.get("device_id") != orig:
                        fail(f"T4b 失败后应保留原设备 {orig}，got {ev and ev.get('device_id')}")
                    print("  [PASS] 不可读设备 -> failed 且保留原设备")
                finally:
                    sudo_shell(f"chmod 660 {node}")

        # ---- T5: 三摄像头反复切换 5 轮 ----
        print("-- T5 反复切换 --")
        for r in range(5):
            for cam in cams:
                before = latest_bytes(offerer) or 0
                offerer.send(f"set_video_device {cam}")
                ev = offerer.wait("video_device", timeout=20)
                if not ev or ev.get("result") != "success":
                    fail(f"T5 第{r}轮切 {cam} 失败: {ev}")
                time.sleep(1.2)
                after = latest_bytes(offerer)
                if after is None or after <= before:
                    fail(f"T5 第{r}轮 {cam} 画面停止")
            if not offerer.alive():
                fail(f"T5 第{r}轮后进程崩溃")
            print(f"  [PASS] 第 {r+1}/5 轮（3 个摄像头）")

        # ---- T6: 切合成再切回真实 ----
        print("-- T6 合成 <-> 真实 --")
        offerer.send("set_video_device synthetic_hd")
        ev = offerer.wait("video_device", timeout=20)
        if not ev or ev.get("result") != "success":
            fail(f"T6 切 synthetic_hd 失败: {ev}")
        if not wait_remote_res(LOGDIR_A, "1280x720", timeout=20):
            fail("T6 远端未收到 1280x720")
        offerer.send(f"set_video_device {cams[0]}")
        ev = offerer.wait("video_device", timeout=20)
        if not ev or ev.get("result") != "success":
            fail(f"T6 切回 {cams[0]} 失败: {ev}")
        print("  [PASS] 合成 <-> 真实切换正常")

        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("media stress ok")
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
