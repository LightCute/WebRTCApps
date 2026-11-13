#!/usr/bin/env python3
"""P2 组合与命令风暴测试。

A. 组合（顺序交叠）：视频 × ICE 切换 / 重连 / 切设备切档 / 热插拔 / 挂断重呼
B. 风暴（背靠背连发）：重复命令 / 语义冲突 / 开关抖动 / 参数刷屏 / 非法刷屏 / 双客户端广播

拓扑：两个本地 daemon（信令云端、媒体 relay）。offerer 用真实摄像头，answerer 用 synthetic。
热插拔用例需要 SUDO_PASS；B6 双客户端不需要。
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
LOGDIR_O = "/tmp/wc_combo_o"
LOGDIR_A = "/tmp/wc_combo_a"
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
            stderr=open(f"/tmp/wc_combo_stderr_{self.role}.log", "w"))

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


class RawClient:
    """第二个 unix socket 客户端（用于 B6 双客户端广播）。"""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(0.5)
        self.sock.connect(path)
        self.f = self.sock.makefile("rw", buffering=1)
        self.pending = []

    def send(self, line):
        self.f.write(line + "\n")
        self.f.flush()

    def wait(self, event, pred=None, timeout=6.0):
        for i, ev in enumerate(self.pending):
            if ev.get("event") == event and (pred is None or pred(ev)):
                return self.pending.pop(i)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.f.readline()
            except (socket.timeout, OSError):
                continue
            if not line:
                time.sleep(0.05)
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("event") == event and (pred is None or pred(ev)):
                return ev
            self.pending.append(ev)
        return None

    def close(self):
        try:
            self.f.close()
            self.sock.close()
        except OSError:
            pass


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
    for f in sorted(os.listdir(log_dir)) if os.path.isdir(log_dir) else []:
        if f.endswith(".log"):
            try:
                with open(os.path.join(log_dir, f), errors="replace") as fh:
                    text += fh.read()
            except OSError:
                pass
    return text


def wait_remote_res(log_dir, expected, timeout=25.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in read_logs(log_dir).splitlines():
            if "Remote video frame" in line and expected in line.split("Remote video frame")[-1]:
                return True
        time.sleep(0.3)
    return False


def newest_stats(d, timeout=10.0):
    return d.wait("video_stats", timeout=timeout)


def assert_frames_continue(d, label):
    d.drain("video_stats")
    time.sleep(2)
    st = newest_stats(d)
    if not st:
        fail(f"{label}: 没有 video_stats")
    before = st.get("bytes_sent", 0)
    time.sleep(2)
    st2 = d.wait("video_stats", timeout=10)
    if not st2 or st2.get("bytes_sent", 0) <= before:
        fail(f"{label}: 画面停止（{before}->{st2 and st2.get('bytes_sent')}）")


def sudo_shell(script):
    password = os.environ.get("SUDO_PASS", "")
    if password:
        r = subprocess.run(["sudo", "-S", "sh", "-c", script],
                           input=password + "\n", text=True, capture_output=True)
    else:
        r = subprocess.run(["sudo", "-n", "sh", "-c", script],
                           text=True, capture_output=True)
    return r.returncode == 0, r.stderr.strip()


def usb_device_for_camera(cam_id):
    """bus_info(usb-0000:00:14.0-8) -> USB 设备名(1-8)。

    用 USB 设备级 unbind/bind 模拟真实拔插：接口级 bind 常残留半绑定状态
    （driver bound 但 video 节点不重建），设备级会完整重新枚举。
    """
    bus = cam_id.split(":", 1)[-1]
    port = bus.split("-")[-1]
    cands = [p for p in glob.glob(f"/sys/bus/usb/devices/*-{port}")
             if os.path.exists(os.path.join(p, "idVendor"))]
    return os.path.basename(cands[0]) if cands else None


def device_nodes_back(cam_id):
    """v4l2-ctl 里该 bus_info 的块中是否已重新出现 /dev/video*。"""
    bus = cam_id.split(":", 1)[-1]
    out = subprocess.run(["v4l2-ctl", "--list-devices"], text=True,
                         capture_output=True).stdout
    in_block = False
    for line in out.splitlines():
        if f"({bus})" in line:
            in_block = True
            continue
        if in_block:
            s = line.strip()
            if s.startswith("/dev/video"):
                return True
            if s and not s.startswith("/dev/"):
                break
    return False


USB_DRIVER = "/sys/bus/usb/drivers/usb"


def rebind_camera(usb_dev, cam_id, attempts=3, timeout=10.0):
    """设备级重绑并轮询 video 节点恢复（等价于物理插回）。"""
    for _ in range(attempts):
        sudo_shell(f"echo {usb_dev} > {USB_DRIVER}/unbind")
        time.sleep(1.0)
        sudo_shell(f"echo {usb_dev} > {USB_DRIVER}/bind")
        deadline = time.time() + timeout
        while time.time() < deadline:
            if device_nodes_back(cam_id):
                return True
            time.sleep(0.5)
    return False


def main():
    for d in (LOGDIR_O, LOGDIR_A):
        subprocess.run(["rm", "-rf", d])
        os.makedirs(d, exist_ok=True)
    offerer = Daemon("offerer", "/tmp/wc_combo_o.sock", LOGDIR_O)
    answerer = Daemon("answerer", "/tmp/wc_combo_a.sock", LOGDIR_A)
    bound_cam = None
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()

        # ---- 准备：选设备 / ICE 模式 / 注册 / 通话 / 开视频 ----
        # offerer 切 host 时，answerer 必须能采集 host 候选（auto），否则云端 relay 够不着私网 host。
        answerer.send("set_ice_transports auto")
        answerer.wait("ice_config", timeout=5)
        offerer.send("set_ice_transports relay")
        offerer.wait("ice_config", timeout=5)
        answerer.send("set_video_device synthetic")
        answerer.wait("video_device", timeout=5)
        offerer.send("get_video_devices")
        ev = offerer.wait("video_devices", timeout=10)
        real = [d for d in ev.get("devices", []) if not d.get("synthetic")]
        if not real:
            fail("没有真实摄像头")
        cams = [d["id"] for d in real]
        cam = cams[0]
        offerer.send(f"set_video_device {cam}")
        if not offerer.wait("video_device", timeout=10):
            fail("选择摄像头失败")

        suffix = str(int(time.time()) % 100000)
        oname, aname = f"cb_o{suffix}", f"cb_a{suffix}"
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

        def call_and_video():
            offerer.drain("peer_connection.state")
            answerer.drain("peer_connection.state")
            offerer.send(f"call {aid}")
            if not offerer.wait("peer_connection.state",
                                lambda e: e.get("state") == "connected", 20):
                fail("通话未连通")
            if not answerer.wait("peer_connection.state",
                                 lambda e: e.get("state") == "connected", 20):
                fail("对端未连通")

        call_and_video()
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=20):
            fail("对端未收到视频")
        offerer.send("set_video_stats on 1000")
        time.sleep(1)
        print("准备完成：通话 + 视频 + 统计")

        # ================= A. 组合 =================
        print("-- A1 视频中 switch_ice --")
        for mode in ("host", "relay"):
            offerer.drain("ice_switched")
            answerer.drain("ice_switched")
            offerer.send(f"switch_ice {mode}")
            if not offerer.wait("ice_switched",
                                lambda e: e.get("result") == "success", 30):
                fail(f"A1 switch_ice {mode} 未成功")
            if not answerer.wait("ice_switched",
                                 lambda e: e.get("result") == "success", 30):
                fail(f"A1 对端 switch_ice {mode} 未成功")
            assert_frames_continue(offerer, f"A1 {mode} 后")
            print(f"  [PASS] switch_ice {mode} 后画面继续")

        print("-- A2 视频中 reconnect ×3 --")
        for i in range(3):
            offerer.drain("reconnected")
            offerer.send("reconnect")
            if not offerer.wait("reconnected",
                                lambda e: e.get("result") == "success", 30):
                fail(f"A2 第{i}次 reconnect 未成功")
        assert_frames_continue(offerer, "A2 后")
        print("  [PASS] reconnect ×3 后画面继续")

        if len(cams) >= 2:
            print("-- A3 切设备后立刻切档位 --")
            offerer.send(f"set_video_device {cams[1]}")
            offerer.send("set_video_profile 640 480 30")
            ev1 = offerer.wait("video_device", timeout=20)
            ev2 = offerer.wait("video_profile", timeout=25)
            if not ev1 or ev1.get("result") != "success":
                fail(f"A3 切设备失败: {ev1}")
            if not ev2 or ev2.get("result") != "success":
                fail(f"A3 切档位失败: {ev2}")
            if not wait_remote_res(LOGDIR_A, "640x480", timeout=25):
                fail("A3 远端未收到 640x480")
            print("  [PASS] 切设备 + 切档位 都成功，远端 640x480")
            offerer.send(f"set_video_device {cam}")
            offerer.wait("video_device", timeout=20)

        print("-- A4 视频中热插拔 --")
        usb_dev = usb_device_for_camera(cam)
        if not usb_dev:
            print("  [SKIP] 找不到 USB 设备节点")
        elif not os.environ.get("SUDO_PASS"):
            print("  [SKIP] 未提供 SUDO_PASS")
        else:
            ok, err = sudo_shell(f"echo {usb_dev} > {USB_DRIVER}/unbind")
            if not ok:
                fail(f"A4 解绑失败: {err}")
            bound_cam = (usb_dev, cam)  # 兜底恢复用
            if not offerer.wait("video_device_lost", timeout=15):
                fail("A4 未收到 video_device_lost")
            sw = offerer.wait("video_switched",
                              lambda e: e.get("enabled") is False, timeout=15)
            if not sw:
                fail("A4 视频通道未关闭")
            if not answerer.wait("remote_video.stopped", timeout=15):
                fail("A4 对端未收到 remote_video.stopped")
            if not rebind_camera(usb_dev, cam):
                fail("A4 重新绑定后设备节点未恢复")
            bound_cam = None
            if not offerer.wait("video_devices_changed",
                                lambda e: e.get("reason") == "device_added",
                                timeout=25):
                fail("A4 未收到 device_added")
            offerer.send("video_on")
            if not answerer.wait("remote_video.started", timeout=25):
                fail("A4 热插拔后视频未恢复")
            time.sleep(2)
            print("  [PASS] 热插拔：关闭 → 插回通知 → 重新开视频")

        print("-- A5 leave 后立刻 call --")
        offerer.send("leave")
        if not offerer.wait("signal_client.call_ended", timeout=10):
            fail("A5 leave 未结束")
        answerer.wait("signal_client.call_ended", timeout=10)
        call_and_video()
        assert_frames_continue(offerer, "A5 重呼后")
        print("  [PASS] leave → call 重连，画面正常")

        # ================= B. 风暴 =================
        print("-- B2 video_on + switch_ice 连发（两个都触发重协商）--")
        offerer.send("video_off")
        offerer.wait("video_switched", timeout=20)
        offerer.send("video_on")
        offerer.send("switch_ice relay")
        connected = False
        for _ in range(10):
            time.sleep(2)
            offerer.drain("peer_connection_status")
            offerer.send("peer_connection_status")
            ev = offerer.wait("peer_connection_status", timeout=5)
            if ev and ev.get("connected"):
                connected = True
                break
        if not connected:
            fail("B2 结束后未处于 connected")
        if not offerer.alive():
            fail("B2 进程崩溃")
        print("  [PASS] 连发后仍 connected、进程存活")

        print("-- B3 video_on/off 抖动 --")
        for line in ("video_on", "video_off", "video_on", "video_off", "video_on"):
            offerer.send(line)
        time.sleep(5)
        offerer.drain("video_params")
        offerer.send("get_video_status")
        ev = offerer.wait("video_params", timeout=5)
        if not ev or ev.get("enabled") is not True:
            fail(f"B3 最终应 enabled=true，got {ev and ev.get('enabled')}")
        if not offerer.alive():
            fail("B3 进程崩溃")
        print("  [PASS] 抖动后最终 enabled=true")

        print("-- B4 set_bitrate 刷屏 ×20 --")
        for i in range(20):
            offerer.send(f"set_bitrate {500 + i * 10}")
        time.sleep(3)
        offerer.drain("video_params")
        offerer.send("get_video_status")
        ev = offerer.wait("video_params", timeout=5)
        if not ev or ev.get("bitrate_kbps") != 690:
            fail(f"B4 应以最后一个值为准(690)，got {ev and ev.get('bitrate_kbps')}")
        if not offerer.alive():
            fail("B4 进程崩溃")
        print("  [PASS] 20 条刷屏后回显为最后值 690")

        print("-- B1 switch_ice 连发 ×3 --")
        for _ in range(3):
            offerer.send("switch_ice host")
        time.sleep(8)
        offerer.send("get_ice_config")
        ev = offerer.wait("ice_config", timeout=5)
        if not ev or ev.get("mode") != "host":
            fail(f"B1 最终 mode 应为 host，got {ev and ev.get('mode')}")
        if not offerer.alive():
            fail("B1 进程崩溃")
        print("  [PASS] 3 连发后 mode=host、进程存活")

        print("-- B6 双客户端广播 --")
        second = RawClient(offerer.sock)
        try:
            if not second.wait("ipc.connected", timeout=6):
                fail("B6 第二个客户端未收到 ipc.connected")
            offerer.drain("signal_client.roster")
            second.send("get_roster")   # 由第二个客户端发命令
            if not offerer.wait("signal_client.roster", timeout=8):
                fail("B6 第一个客户端未收到第二个客户端命令的 roster（未广播）")
            if not second.wait("signal_client.roster", timeout=8):
                fail("B6 第二个客户端未收到自己的 roster")
            offerer.drain("video_params")
            second.send("get_video_status")
            if not offerer.wait("video_params", timeout=8):
                fail("B6 B 的命令回执未广播给 A")
            offerer.drain("video_device")
            second.send("set_video_device synthetic")
            ev = second.wait("video_device", timeout=10)
            if not ev:
                fail("B6 第二个客户端控制失败")
            if not offerer.wait("video_device", timeout=8):
                fail("B6 第二个客户端切设备的事件未广播给 A")
            print("  [PASS] 双客户端：任意端可控制，事件广播")
        finally:
            second.close()
        time.sleep(0.5)
        offerer.drain("signal_client.roster")
        offerer.send("get_roster")
        if not offerer.wait("signal_client.roster", timeout=8):
            fail("B6 第二客户端断开后，第一个客户端不可用")
        if not offerer.alive():
            fail("B6 断开第二客户端后进程崩溃")
        print("  [PASS] 第二客户端断开后第一个仍正常")

        print("-- B5 非法命令刷屏 ×100 --")
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        for i in range(100):
            if i % 3 == 0:
                offerer.send("call 0")
            elif i % 3 == 1:
                offerer.send("switch_ice")
            else:
                offerer.send("set_video_device x")
        time.sleep(3)
        offerer.drain("ipc.error")
        offerer.send("call 0")
        if not offerer.wait("ipc.error", timeout=5):
            fail("B5 非法命令未返回 ipc.error")
        call_and_video()
        print("  [PASS] 100 条非法命令后正常 call 仍成功")
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)

        print("-- 状态复位检查 --")
        offerer.send("peer_connection_status")
        ev = offerer.wait("peer_connection_status", timeout=5)
        if ev and (ev.get("connected") or ev.get("peer_id", 0) > 0):
            fail(f"结束时状态未复位: {ev}")
        if not (offerer.alive() and answerer.alive()):
            fail("进程崩溃")
        print("  [PASS] 状态复位、进程存活")

        print("combo stress ok")
    finally:
        # 兜底：若中途失败时摄像头仍处于解绑状态，恢复它。
        if bound_cam:
            rebind_camera(*bound_cam)
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
