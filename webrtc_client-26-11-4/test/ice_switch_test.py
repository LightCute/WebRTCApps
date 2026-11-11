#!/usr/bin/env python3
"""ICE 模式切换测试：set_ice_transports / switch_ice / get_ice_config。

两个本地 daemon，信令服务器用云端 120.79.210.6:8888，TURN 用云端 coturn。

设计：answerer 固定为 auto（host/srflx/relay 都采集），这样 offerer 无论切到
host 还是 relay 都有可配对候选，连接能重建成功。被测点是：
- switch_ice 只切换【被命令端】的 mode（重建 PC），对端 mode 不变；
- 切换后旧 relay 分配被释放（local_candidates 里不再有 relay）；
- offerer / answerer 两个方向都覆盖。
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


def selected_type(d):
    d.send("get_stats")
    ev = d.wait("stats", timeout=5)
    return (ev or {}).get("candidate_pair", {}).get(
        "local_candidate", {}).get("candidate_type", "")


def local_candidate_types(d):
    d.send("get_stats")
    ev = d.wait("stats", timeout=5)
    if not ev:
        return []
    return [c.get("candidate_type", "") for c in ev.get("local_candidates", [])]


def ice_mode(d):
    d.send("get_ice_config")
    ev = d.wait("ice_config", timeout=5)
    return (ev or {}).get("mode", "")


def wait_switched(*daemons, timeout=25.0):
    deadline = time.time() + timeout
    ok = {d: False for d in daemons}
    while time.time() < deadline:
        for d in list(ok):
            if not ok[d] and d.wait("ice_switched",
                                    lambda e: e.get("result") == "success", 0.3):
                ok[d] = True
        if all(ok.values()):
            return True
    return False


def wait_reconnected(*daemons, timeout=25.0):
    deadline = time.time() + timeout
    ok = {d: False for d in daemons}
    while time.time() < deadline:
        for d in list(ok):
            if not ok[d] and d.wait("reconnected",
                                    lambda e: e.get("result") == "success", 0.3):
                ok[d] = True
        if all(ok.values()):
            return True
    return False


def wait_selected(d, expected, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if selected_type(d) == expected:
            return True
        time.sleep(0.5)
    return False


def main():
    offerer = Daemon("offerer", "/tmp/wc_ice_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_ice_a.sock")
    try:
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
        time.sleep(0.5)  # 等 roster 刷新

        # T0: get_ice_config 回显
        offerer.send("get_ice_config")
        ev = offerer.wait("ice_config")
        print("T0 get_ice_config:", ev.get("mode"), ev.get("turn"))

        # T1: answerer auto + offerer relay，建立通话
        answerer.send("set_ice_transports auto")
        offerer.send("set_ice_transports relay")
        answerer.wait("ice_config")
        offerer.wait("ice_config")
        offerer.send("get_roster")
        ev = offerer.wait("signal_client.roster")
        aid = next(p["id"] for p in ev.get("peers", []) if p["name"] == "answerer")
        offerer.send(f"call {aid}")
        if not wait_selected(offerer, "relay"):
            fail("T1 relay call not connected (offerer)")
        print("T1 offerer=relay answerer=auto connected")

        # T2: offerer switch_ice host（relay->host；answerer 保持 auto）
        offerer.send("switch_ice host")
        if not wait_switched(offerer, answerer):
            fail("T2 switch_ice host: no ice_switched")
        if not wait_selected(offerer, "host"):
            fail("T2 offerer selected != host")
        if "relay" in local_candidate_types(offerer):
            fail("T2 offerer still has relay candidate (old TURN allocation not released)")
        if ice_mode(offerer) != "host":
            fail(f"T2 offerer mode != host: {ice_mode(offerer)}")
        if ice_mode(answerer) != "auto":
            fail(f"T2 answerer mode changed: {ice_mode(answerer)}")
        print("T2 offerer switch host OK: offerer=host(no relay), answerer=auto unchanged")
        time.sleep(1)

        # T3: offerer switch_ice relay（host->relay）
        offerer.send("switch_ice relay")
        if not wait_switched(offerer, answerer):
            fail("T3 switch_ice relay: no ice_switched")
        if not wait_selected(offerer, "relay"):
            fail("T3 offerer selected != relay")
        print("T3 offerer switch relay OK")
        time.sleep(1)

        # T4: answerer switch_ice relay（auto->relay；offerer 保持 relay）
        answerer.send("switch_ice relay")
        if not wait_switched(offerer, answerer):
            fail("T4 switch_ice relay (answerer): no ice_switched")
        if not wait_selected(answerer, "relay"):
            fail("T4 answerer selected != relay")
        if ice_mode(answerer) != "relay":
            fail(f"T4 answerer mode != relay: {ice_mode(answerer)}")
        if ice_mode(offerer) != "relay":
            fail(f"T4 offerer mode changed: {ice_mode(offerer)}")
        print("T4 answerer switch relay OK: answerer=relay, offerer=relay unchanged")

        # T4.5: offerer reconnect（复用旧候选的 ICE 重启，不重建 PC、不改 mode）
        offerer.send("reconnect")
        if not wait_reconnected(offerer):
            fail("T4.5 offerer reconnect: no reconnected")
        if ice_mode(offerer) != "relay":
            fail(f"T4.5 offerer mode changed: {ice_mode(offerer)}")
        if not wait_selected(offerer, "relay"):
            fail("T4.5 offerer selected != relay after reconnect")
        print("T4.5 offerer reconnect OK (mode unchanged, selected=relay)")

        # T4.6: answerer reconnect（answerer 被命令 → 通知 offerer 发起 ICE 重启）
        answerer.send("reconnect")
        if not wait_reconnected(answerer, offerer):
            fail("T4.6 answerer reconnect: no reconnected")
        if ice_mode(answerer) != "relay":
            fail(f"T4.6 answerer mode changed: {ice_mode(answerer)}")
        print("T4.6 answerer reconnect OK")

        # T5: leave
        offerer.send("leave")
        offerer.wait("signal_client.call_ended", timeout=10)
        answerer.wait("signal_client.call_ended", timeout=10)
        time.sleep(0.5)

        # T6: 空闲 switch_ice -> not_in_call
        offerer.send("switch_ice relay")
        ev = offerer.wait("ipc.error")
        print("T6 switch not in call ->", ev.get("code") if ev else None)
        if not ev or ev.get("code") != "not_in_call":
            fail(f"T6 expected not_in_call, got {ev.get('code') if ev else None}")

        print("ice switch ok")
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
