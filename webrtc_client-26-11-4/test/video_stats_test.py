#!/usr/bin/env python3
"""视频统计上报 + 强制关键帧测试。

验证：
1. set_video_stats on 后周期性收到 video_stats（fps/码率/字节数递增）；
2. request_keyframe 成功，且 key_frames_encoded 增长；
3. set_video_stats off 后不再推送；
4. 视频关闭时 request_keyframe 报 failed。
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
            [BIN, f"--socket_path={self.sock}", f"--env_file={ENV}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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


def main():
    offerer = Daemon("offerer", "/tmp/wc_stats_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_stats_a.sock")
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        # 两个本地 daemon 用合成设备（确定性）
        for d in (offerer, answerer):
            d.send("set_video_device synthetic")
            d.wait("video_device", timeout=5)
        suffix = str(int(time.time()) % 100000)
        oname, aname = f"st_o{suffix}", f"st_a{suffix}"
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
        print("T0 通话 + 视频就绪")

        # T1: 开启统计
        offerer.send("set_video_stats on 1000")
        s1 = offerer.wait("video_stats", timeout=8)
        s2 = offerer.wait("video_stats", timeout=8)
        if not s1 or not s2:
            fail(f"T1 未收到足够的 video_stats: {s1}, {s2}")
        print(f"T1 video_stats: {s1['width']}x{s1['height']} fps={s1['fps']:.1f} "
              f"target={int(s1['target_bitrate']/1000)}kbps "
              f"bytes={s1['bytes_sent']} rtt={s1['rtt_ms']:.1f}ms "
              f"pli={s1['pli_count']} nack={s1['nack_count']}")
        if s1["fps"] <= 0:
            fail("T1 fps 应 > 0")
        if s2["bytes_sent"] <= s1["bytes_sent"]:
            fail("T1 bytes_sent 应递增")

        # T2: 强制关键帧 → key_frames_encoded 增长
        before = s2["key_frames_encoded"]
        offerer.send("request_keyframe")
        ev = offerer.wait("keyframe_requested", timeout=5)
        if not ev or ev.get("result") != "success":
            fail(f"T2 request_keyframe 失败: {ev}")
        increased = False
        for _ in range(4):
            st = offerer.wait("video_stats", timeout=8)
            if st and st["key_frames_encoded"] > before:
                increased = True
                print(f"T2 关键帧已生成: {before} -> {st['key_frames_encoded']}")
                break
        if not increased:
            fail("T2 key_frames_encoded 未增长")

        # T3: 关闭统计 → 不再推送
        offerer.send("set_video_stats off")
        time.sleep(0.5)
        while not offerer.q.empty():
            try:
                offerer.q.get_nowait()
            except queue.Empty:
                break
        offerer.pending = []
        if offerer.wait("video_stats", timeout=3) is not None:
            fail("T3 关闭统计后仍在推送")
        print("T3 关闭统计后无推送 OK")

        # T4: 视频关闭时请求关键帧 → failed
        offerer.send("video_off")
        offerer.wait("video_switched", timeout=15)
        offerer.send("request_keyframe")
        ev = offerer.wait("keyframe_requested", timeout=5)
        if not ev or ev.get("result") != "failed":
            fail(f"T4 视频关闭时 request_keyframe 应 failed，got {ev}")
        print("T4 视频关闭时 request_keyframe -> failed OK")

        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("video stats test ok")
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
