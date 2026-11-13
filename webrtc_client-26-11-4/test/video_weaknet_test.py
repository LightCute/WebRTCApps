#!/usr/bin/env python3
"""弱网自适应测试：用 tc netem 对「本机 → 云端 coturn:3478」的 UDP 流量劣化。

因为我们用 relay 模式，媒体全部走 TURN:3478，所以按目标端口过滤整形不会影响
SSH(22) / 信令(8888 TCP)。

需要 sudo：SUDO_PASS=<密码> python3 video_weaknet_test.py

用例：
  S0 基线         → fps / target_bitrate 正常
  S1 丢包 10%     → packets_lost 增长（或 nack 增长 / fps 下降）
  S2 带宽 500kbit → target_bitrate 明显下降
  S3 延迟 200ms   → rtt_ms 明显升高
  S4 恢复         → rtt 回落
"""

import json
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time

BIN = "/home/light/webrtc/checkout/src/out/webrtc_client_x64/webrtc_engine_main"
ENV = "/home/light/webrtc/checkout/src/apps/webrtc_client-26-11-4/.env"
SERVER = "120.79.210.6"
PORT = 8888
TURN_PORT = 3478


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


def sudo_shell(script):
    password = os.environ.get("SUDO_PASS", "")
    if password:
        r = subprocess.run(["sudo", "-S", "sh", "-c", script],
                           input=password + "\n", text=True, capture_output=True)
    else:
        r = subprocess.run(["sudo", "-n", "sh", "-c", script],
                           text=True, capture_output=True)
    return r.returncode == 0, r.stderr.strip()


def detect_iface():
    r = subprocess.run(["ip", "route", "get", SERVER], text=True,
                       capture_output=True)
    m = re.search(r"\bdev\s+(\S+)", r.stdout)
    return m.group(1) if m else None


def tc_clear(iface):
    if iface:
        sudo_shell(f"tc qdisc del dev {iface} root 2>/dev/null || true")


def tc_apply(iface, netem_args):
    tc_clear(iface)
    cmds = [
        f"tc qdisc add dev {iface} root handle 1: prio",
        f"tc filter add dev {iface} parent 1: protocol ip u32 "
        f"match ip dport {TURN_PORT} 0xffff flowid 1:1",
        f"tc qdisc add dev {iface} parent 1:1 handle 10: netem {netem_args}",
    ]
    for c in cmds:
        ok, err = sudo_shell(c)
        if not ok:
            fail(f"tc 失败: {c}: {err}")


def collect_stats(d, seconds):
    samples = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        ev = d.wait("video_stats", timeout=1.0)
        if ev:
            samples.append(ev)
    return samples


def summarize(samples):
    if not samples:
        return None
    last = samples[-1]
    return {
        "fps": last.get("fps", 0),
        "target": last.get("target_bitrate", 0),
        "lost": last.get("packets_lost", 0),
        "nack": last.get("nack_count", 0),
        "rtt": last.get("rtt_ms", 0),
    }


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
    iface = detect_iface()
    if not iface:
        fail("无法探测到出口网卡")
    print(f"出口网卡: {iface}（只整形 dport {TURN_PORT} 的 UDP）")

    offerer = Daemon("offerer", "/tmp/wc_weak_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_weak_a.sock")
    try:
        tc_clear(iface)
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        for d in (offerer, answerer):
            d.send("set_video_device synthetic")
            d.wait("video_device", timeout=5)
        suffix = str(int(time.time()) % 100000)
        oname, aname = f"wk_o{suffix}", f"wk_a{suffix}"
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
        offerer.send("set_video_stats on 1000")
        time.sleep(2)  # 让统计稳定

        # S0 基线
        base = summarize(collect_stats(offerer, 6))
        print(f"S0 基线: fps={base['fps']:.1f} target={int(base['target']/1000)}kbps "
              f"lost={base['lost']} rtt={base['rtt']:.0f}ms")
        if base["fps"] <= 5:
            fail(f"S0 基线 fps 过低: {base['fps']}")

        # S1 丢包 10%
        tc_apply(iface, "loss 10%")
        time.sleep(2)
        loss = summarize(collect_stats(offerer, 8))
        print(f"S1 丢包10%: fps={loss['fps']:.1f} target={int(loss['target']/1000)}kbps "
              f"lost={loss['lost']} nack={loss['nack']}")
        degraded = (loss["lost"] > base["lost"] or loss["nack"] > base["nack"]
                    or loss["fps"] < base["fps"] * 0.8)
        if not degraded:
            fail(f"S1 未见劣化: base={base} loss={loss}")
        print("S1 丢包劣化已观测到 OK")

        # S2 带宽 500kbit
        tc_apply(iface, "rate 500kbit")
        time.sleep(2)
        bw = summarize(collect_stats(offerer, 8))
        print(f"S2 限速500k: fps={bw['fps']:.1f} target={int(bw['target']/1000)}kbps")
        if bw["target"] >= 800000:
            fail(f"S2 目标码率未下降: {bw['target']}")
        print("S2 带宽自适应下降 OK")

        # S3 延迟 200ms
        tc_apply(iface, "delay 200ms 50ms")
        time.sleep(2)
        delay = summarize(collect_stats(offerer, 8))
        print(f"S3 延迟200ms: rtt={delay['rtt']:.0f}ms fps={delay['fps']:.1f}")
        if delay["rtt"] < 150:
            fail(f"S3 RTT 未升高: {delay['rtt']}")
        print("S3 延迟升高可观测 OK")

        # S4 恢复
        tc_clear(iface)
        time.sleep(2)
        recovered = summarize(collect_stats(offerer, 8))
        print(f"S4 恢复: rtt={recovered['rtt']:.0f}ms "
              f"target={int(recovered['target']/1000)}kbps fps={recovered['fps']:.1f}")
        if recovered["rtt"] >= delay["rtt"]:
            fail(f"S4 RTT 未回落: {recovered['rtt']} vs {delay['rtt']}")
        print("S4 网络恢复后 RTT 回落 OK")

        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        print("weaknet test ok")
    finally:
        tc_clear(iface)
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
