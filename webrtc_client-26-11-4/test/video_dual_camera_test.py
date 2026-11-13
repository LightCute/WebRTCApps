#!/usr/bin/env python3
"""双摄像头视频通话测试：两个本地 daemon 各用一个真实摄像头互发视频。

验证：
1. 枚举到 2 个真实摄像头；
2. 两个 daemon 分别打开不同摄像头（日志确认）；
3. 双方互收远端视频、帧计数持续增长；
4. 通话中互换摄像头不崩溃。
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
LOGDIR_O = "/tmp/wc_dual_o"
LOGDIR_A = "/tmp/wc_dual_a"


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
            stderr=open(f"/tmp/wc_dual_stderr_{self.role}.log", "w"))

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


def max_frame_count(log_dir):
    """解析日志里 "Remote video frame #N"，返回最大 N。"""
    best = 0
    for line in read_logs(log_dir).splitlines():
        if "Remote video frame #" in line:
            try:
                n = int(line.split("Remote video frame #")[1].split()[0])
                best = max(best, n)
            except (IndexError, ValueError):
                pass
    return best


def main():
    for d in (LOGDIR_O, LOGDIR_A):
        subprocess.run(["rm", "-rf", d])
        os.makedirs(d, exist_ok=True)
    offerer = Daemon("offerer", "/tmp/wc_dual_o.sock", LOGDIR_O)
    answerer = Daemon("answerer", "/tmp/wc_dual_a.sock", LOGDIR_A)
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()

        # T0: 枚举，找到 2 个真实摄像头
        offerer.send("get_video_devices")
        ev = offerer.wait("video_devices", timeout=10)
        if not ev:
            fail("T0 未收到 video_devices")
        real = [d for d in ev.get("devices", []) if not d.get("synthetic")]
        print(f"T0 真实摄像头 {len(real)} 个: {[d['id'] for d in real]}")
        if len(real) < 2:
            fail(f"T0 需要 2 个真实摄像头，实际 {len(real)} 个")
        cam_a, cam_b = real[0]["id"], real[1]["id"]

        # T1: 两个 daemon 分别选不同摄像头
        offerer.send(f"set_video_device {cam_a}")
        ev = offerer.wait("video_device", timeout=5)
        if not ev or ev.get("result") != "success":
            fail(f"T1 offerer 选设备失败: {ev}")
        answerer.send(f"set_video_device {cam_b}")
        ev = answerer.wait("video_device", timeout=5)
        if not ev or ev.get("result") != "success":
            fail(f"T1 answerer 选设备失败: {ev}")
        print(f"T1 offerer->{cam_a}, answerer->{cam_b}")

        # T2: 注册 + 通话
        suffix = str(int(time.time()) % 100000)
        oname, aname = f"offerer{suffix}", f"answerer{suffix}"
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
        print("T2 call connected")

        # T3: 双方开视频，互收远端视频
        offerer.send("video_on")
        answerer.send("video_on")
        if not offerer.wait("remote_video.started", timeout=20):
            fail("T3 offerer 未收到远端视频轨")
        if not answerer.wait("remote_video.started", timeout=20):
            fail("T3 answerer 未收到远端视频轨")
        print("T3 双方均收到远端视频轨")

        # T4: 帧持续流动（等待 5 秒，两边帧计数都应 > 60）
        time.sleep(5)
        o_frames = max_frame_count(LOGDIR_O)
        a_frames = max_frame_count(LOGDIR_A)
        print(f"T4 远端帧计数: offerer={o_frames}, answerer={a_frames}")
        if o_frames < 60 or a_frames < 60:
            fail(f"T4 帧数不足（5 秒内应 >60）: {o_frames}/{a_frames}")

        # T5: 确认两边用的是不同摄像头
        o_log, a_log = read_logs(LOGDIR_O), read_logs(LOGDIR_A)
        if f"Video source created with device: {cam_a}" not in o_log:
            fail(f"T5 offerer 未使用 {cam_a}")
        if f"Video source created with device: {cam_b}" not in a_log:
            fail(f"T5 answerer 未使用 {cam_b}")
        print("T5 两个 daemon 确认使用不同摄像头")

        # T6a: 通话中切到空闲设备（合成）→ 成功，画面继续
        before_o = max_frame_count(LOGDIR_O)
        offerer.send("set_video_device synthetic_hd")
        ev = offerer.wait("video_device", timeout=20)
        if not ev or ev.get("result") != "success":
            fail(f"T6a 切到 synthetic_hd 失败: {ev}")
        time.sleep(4)
        after_o = max_frame_count(LOGDIR_O)
        print(f"T6a offerer 帧计数 {before_o} -> {after_o}（切到 synthetic_hd）")
        if after_o <= before_o:
            fail("T6a 切到 synthetic_hd 后画面停止")

        # T6b: 切回自己刚释放的摄像头 A → 成功（A 已空闲）
        offerer.send(f"set_video_device {cam_a}")
        ev = offerer.wait("video_device", timeout=20)
        if not ev or ev.get("result") != "success":
            fail(f"T6b 切回 {cam_a} 失败: {ev}")
        time.sleep(3)
        print("T6b 切回原摄像头 OK")

        # T6c: 切到对端正在使用的摄像头 B → 优雅失败，保留原设备
        offerer.send(f"set_video_device {cam_b}")
        ev = offerer.wait("video_device", timeout=20)
        if not ev:
            fail("T6c 未收到 video_device 结果")
        if ev.get("result") != "failed":
            fail(f"T6c 占用中的摄像头本应失败，got {ev}")
        if ev.get("id") != cam_a:
            fail(f"T6c 失败后应保留原设备 {cam_a}，got {ev.get('id')}")
        print("T6c 切到对端占用中的摄像头 -> failed 且保留原设备 OK")

        if not (offerer.alive() and answerer.alive()):
            fail("T6 切换过程后进程崩溃")
        # 失败后画面仍应继续（沿用原设备）
        before_o = max_frame_count(LOGDIR_O)
        time.sleep(3)
        if max_frame_count(LOGDIR_O) <= before_o:
            fail("T6c 切换失败后画面停止")
        print("T6 切换场景全部 OK（无崩溃、画面持续）")

        # 收尾
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("dual camera test ok")
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
