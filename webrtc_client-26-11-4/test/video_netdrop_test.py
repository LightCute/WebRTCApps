#!/usr/bin/env python3
"""网络断开测试：用 iptables 精确屏蔽端口模拟断网。

iptables 规则放在独立链 WEBRTC_TEST 里，绝不碰用户已有规则；finally 无条件清理。

用例：
  N1 媒体瞬断 6s（UDP → 云端，relay 全走 3478）
     → peer_connection.state 变 disconnected → 恢复后自动重连、画面恢复
  N2 媒体断 45s
     → 可能升级 failed（引擎自动 leave，双方 call_ended）；恢复后能重新 call
  N3 信令断开 6s（TCP 8888）
     → signal_client.disconnected；恢复后重新 register + call 成功

需要 sudo：SUDO_PASS=<密码> python3 video_netdrop_test.py
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
CHAIN = "WEBRTC_TEST"


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
            stdout=subprocess.DEVNULL,
            stderr=open(f"/tmp/wc_netdrop_{self.role}.log", "w"))

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


def sudo_shell(script):
    password = os.environ.get("SUDO_PASS", "")
    if password:
        r = subprocess.run(["sudo", "-S", "sh", "-c", script],
                           input=password + "\n", text=True, capture_output=True)
    else:
        r = subprocess.run(["sudo", "-n", "sh", "-c", script],
                           text=True, capture_output=True)
    return r.returncode == 0, r.stderr.strip()


def net_clear():
    sudo_shell(f"iptables -w -D OUTPUT -j {CHAIN} 2>/dev/null || true")
    sudo_shell(f"iptables -w -F {CHAIN} 2>/dev/null || true")
    sudo_shell(f"iptables -w -X {CHAIN} 2>/dev/null || true")


def net_block(spec):
    """spec 例：'-p udp -d 120.79.210.6'。"""
    ok, _ = sudo_shell(f"iptables -w -N {CHAIN} 2>/dev/null || true")
    sudo_shell(f"iptables -w -C OUTPUT -j {CHAIN} 2>/dev/null || "
               f"iptables -w -I OUTPUT -j {CHAIN}")
    ok, err = sudo_shell(f"iptables -w -A {CHAIN} {spec} -j DROP")
    if not ok:
        fail(f"iptables 添加失败: {err}")


def restart_coturn():
    """清云端 coturn 的残留 TURN 分配（长断开后旧分配释放不掉）。"""
    subprocess.run(["ssh", "-o", "StrictHostKeyChecking=no",
                    "-o", "ConnectTimeout=10", f"root@{SERVER}",
                    "systemctl restart coturn"], capture_output=True)
    time.sleep(2)


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


def ensure_video_on(d):
    d.drain("video_params")
    d.send("get_video_status")
    ev = d.wait("video_params", timeout=5)
    if not ev or ev.get("enabled") is not True:
        d.send("video_on")


def take_fresh_bytes(d, wait_s=2.0, timeout=10.0):
    d.drain("video_stats")
    time.sleep(wait_s)
    st = d.wait("video_stats", timeout=timeout)
    return st.get("bytes_sent", 0) if st else None


def main():
    if not os.environ.get("SUDO_PASS"):
        fail("需要 SUDO_PASS=<密码>")
    offerer = Daemon("offerer", "/tmp/wc_nd_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_nd_a.sock")
    try:
        net_clear()
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        for d in (offerer, answerer):
            d.send("set_video_device synthetic")
            d.wait("video_device", timeout=5)
            d.send("set_ice_transports relay")
            d.wait("ice_config", timeout=5)
        suffix = str(int(time.time()) % 100000)
        oname, aname = f"nd_o{suffix}", f"nd_a{suffix}"
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

        def call():
            offerer.drain("peer_connection.state")
            answerer.drain("peer_connection.state")
            offerer.send(f"call {aid}")
            ok = offerer.wait("peer_connection.state",
                              lambda e: e.get("state") == "connected", 25)
            if not ok:
                fail("呼叫未连通")
            answerer.wait("peer_connection.state",
                          lambda e: e.get("state") == "connected", 25)

        call()
        offerer.send("video_on")
        if not answerer.wait("remote_video.started", timeout=25):
            fail("对端未收到视频")
        offerer.send("set_video_stats on 1000")
        time.sleep(2)
        base = take_fresh_bytes(offerer)
        print(f"基线 bytes={base}")

        # ---------- N1: 媒体瞬断 6s ----------
        print("-- N1 媒体瞬断 6s --")
        net_block(f"-p udp -d {SERVER}")
        state = offerer.wait("peer_connection.state",
                             lambda e: e.get("state") in ("disconnected", "failed"),
                             timeout=25)
        if not state:
            fail("N1 断网后未观测到 disconnected/failed")
        print(f"  [PASS] 断网后状态={state.get('state')}")
        time.sleep(3)
        net_clear()
        print("  网络已恢复，等待自动重连…")
        rec = offerer.wait("peer_connection.state",
                           lambda e: e.get("state") == "connected", timeout=30)
        if not rec:
            fail("N1 恢复网络后未自动重连")
        time.sleep(2)
        after = take_fresh_bytes(offerer)
        if after is None or after <= base:
            fail(f"N1 重连后画面未恢复（{base}->{after}）")
        print(f"  [PASS] 自动重连、画面恢复（bytes {base}->{after}）")
        base = after

        # ---------- N2: 媒体断 45s ----------
        print("-- N2 媒体断 45s --")
        net_block(f"-p udp -d {SERVER}")
        deadline = time.time() + 50
        ended = False
        while time.time() < deadline:
            ev = offerer.wait("signal_client.call_ended", timeout=2)
            if ev:
                ended = True
                break
            # 也确认进程还活着
            if not offerer.alive():
                fail("N2 断网期间进程崩溃")
        net_clear()
        time.sleep(1)
        if not ended:
            fail("N2 长时间断开后未挂断（预期 call_ended）")
        print("  [PASS] 长时间断开 → 通话被清理（call_ended）")
        answerer.wait("signal_client.call_ended", timeout=10)
        # 挂断后状态必须复位（不残留 in_call）
        offerer.drain("peer_connection_status")
        offerer.send("peer_connection_status")
        ev = offerer.wait("peer_connection_status", timeout=5)
        if not ev or ev.get("connected") or ev.get("peer_id", 0) > 0:
            fail(f"N2 挂断后状态未复位: {ev}")
        if not (offerer.alive() and answerer.alive()):
            fail("N2 断网期间进程崩溃")
        print("  [PASS] 挂断后状态已复位、进程存活")

        print("-- N2b 不重启 coturn，直接重呼（端口窗口轮换）--")
        # 每次新建 PC 会换一组本地端口，避开上一通失败时残留的 TURN 分配（437）。
        call()
        ensure_video_on(offerer)
        if not answerer.wait("remote_video.started", timeout=30):
            fail("N2b 重呼后视频未恢复")
        offerer.send("set_video_stats on 1000")
        time.sleep(2)
        print("  [PASS] 重呼成功、视频恢复")

        # ---------- N3: 信令断开 6s ----------
        print("-- N3 信令断开 6s --")
        offerer.drain("signal_client.disconnected")
        # 服务端 ping_timeout 默认 30s：断链后要等它判定超时并关闭连接。
        net_block(f"-p tcp -d {SERVER} --dport {PORT}")
        disc = offerer.wait("signal_client.disconnected", timeout=50)
        if not disc:
            fail("N3 未收到 signal_client.disconnected")
        print("  [PASS] 信令断开事件已收到")
        time.sleep(2)
        net_clear()
        time.sleep(1)
        # 重新注册 + 通话（信令断开后引擎会 ClosePeerConnection）
        new_name = f"nd_o{suffix}b"
        offerer.send(f"register {new_name} {SERVER} {PORT}")
        if not offerer.wait("signal_client.signed_in", timeout=15):
            fail("N3 恢复后重新注册失败")
        answerer.send(f"register {aname}b {SERVER} {PORT}")
        if not answerer.wait("signal_client.signed_in", timeout=15):
            fail("N3 对端重新注册失败")
        time.sleep(0.5)
        aid2 = peer_id_of(offerer, f"{aname}b")
        if aid2 <= 0:
            fail("N3 恢复后找不到对端")
        offerer.drain("peer_connection.state")
        answerer.drain("peer_connection.state")
        offerer.send(f"call {aid2}")
        if not offerer.wait("peer_connection.state",
                            lambda e: e.get("state") == "connected", 25):
            fail("N3 恢复后 call 失败")
        print("  [PASS] 信令恢复后重新注册 + call 成功")

        if not (offerer.alive() and answerer.alive()):
            fail("进程崩溃")
        print("netdrop test ok")
    finally:
        net_clear()
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
