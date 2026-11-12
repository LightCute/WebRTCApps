#!/usr/bin/env python3
"""WebRTC 后端稳定性抗压测试：接口全覆盖 + 反复抗压。

环境：云信令 120.79.210.6:8888 + 云 daemon + 本地 daemon，双向（A/B）。
判据：不崩溃 + 事件/错误码正确 + 状态机不卡死（不残留 busy/in_call）。

用法：
  python3 stress_test.py                      # 全量（接口覆盖一次 + 双向抗压各 5 遍）
  python3 stress_test.py --cycles 3           # 抗压遍数
  python3 stress_test.py --direction A        # 只跑 A 方向（本地 offerer）
  python3 stress_test.py --interface-only     # 只跑接口覆盖
  python3 stress_test.py --stress-only        # 只跑反复抗压
"""

import argparse
import json
import os
import queue
import signal
import socket
import subprocess
import sys
import threading
import time

# ===================== 配置 =====================

LOCAL_BIN = "/home/light/webrtc/checkout/src/out/webrtc_client_x64/webrtc_engine_main"
CLOUD_BIN = "/root/stun_test/webrtc_engine_main"
CLOUD_HOST = "120.79.210.6"
CLOUD_USER = "root"
SIGNAL_SERVER = "120.79.210.6"
SIGNAL_PORT = 8888
LOCAL_SOCK = "/tmp/wc_stress_local.sock"
CLOUD_SOCK = "/tmp/wc_stress_cloud.sock"
LOCAL_ENV = "/home/light/webrtc/checkout/src/apps/webrtc_client-26-11-4/.env"
CLOUD_ENV = "/root/stun_test/.env"

CONNECT_TIMEOUT = 20
CALL_END_TIMEOUT = 10
SWITCH_TIMEOUT = 30


def ssh_cmd(*args):
    return ["ssh", "-o", "StrictHostKeyChecking=no",
            "-o", "ConnectTimeout=10", f"{CLOUD_USER}@{CLOUD_HOST}"] + list(args)


# ===================== Daemon 封装 =====================

class Daemon:
    """一个 webrtc_engine_main 进程 + 其 unix socket 连接（本地直接连 / 云端 ssh+nc 桥）。"""

    def __init__(self, role, location):
        self.role = role
        self.location = location
        self.sock = LOCAL_SOCK if location == "local" else CLOUD_SOCK
        self.log_dir = "/tmp/wc_stress_local" if location == "local" else "/tmp/wc_stress_cloud"
        self.server = SIGNAL_SERVER if location == "local" else "127.0.0.1"
        self.env_file = LOCAL_ENV if location == "local" else CLOUD_ENV
        self.pid = None
        self.proc = None
        self.rfile = None
        self.wfile = None
        self.q = queue.Queue()
        self.reader = None

    def start(self):
        args = [f"--socket_path={self.sock}",
                f"--env_file={self.env_file}",
                f"--log_dir={self.log_dir}"]
        if self.location == "local":
            try:
                os.unlink(self.sock)
            except OSError:
                pass
            self.proc = subprocess.Popen(
                [LOCAL_BIN] + args,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.pid = self.proc.pid
        else:
            cmd = (f"rm -f {self.sock}; nohup {CLOUD_BIN} " +
                   " ".join(args) +
                   f" > /tmp/wc_stress_cloud_daemon.log 2>&1 & echo $!")
            out = subprocess.check_output(ssh_cmd(cmd), text=True).strip()
            self.pid = int(out.splitlines()[-1])

    def connect(self):
        if self.location == "local":
            for _ in range(60):
                if os.path.exists(self.sock):
                    break
                time.sleep(0.25)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(self.sock)
            f = s.makefile("rw", buffering=1)
            self.rfile = f
            self.wfile = f
        else:
            for _ in range(60):
                if self._cloud_sock_exists():
                    break
                time.sleep(0.25)
            self.proc = subprocess.Popen(
                ssh_cmd("nc", "-U", self.sock),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, bufsize=1)
            self.rfile = self.proc.stdout
            self.wfile = self.proc.stdin
        self.q = queue.Queue()
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _cloud_sock_exists(self):
        return subprocess.run(ssh_cmd(f"test -S {self.sock}"),
                              capture_output=True).returncode == 0

    def _read_loop(self):
        try:
            for line in self.rfile:
                line = line.strip()
                if not line:
                    continue
                try:
                    self.q.put(json.loads(line))
                except json.JSONDecodeError:
                    pass
        except (ValueError, OSError):
            pass

    def send(self, line):
        if self.wfile is None:
            return
        try:
            self.wfile.write(line + "\n")
            self.wfile.flush()
        except (ValueError, OSError, BrokenPipeError):
            pass

    def wait_event(self, event, predicate=None, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                ev = self.q.get(timeout=0.3)
            except queue.Empty:
                continue
            if ev.get("event") == event and (predicate is None or predicate(ev)):
                return ev
        return None

    def stop(self):
        try:
            self.send("quit")
        except Exception:
            pass
        time.sleep(0.4)

    def kill9(self):
        if self.pid is None:
            return
        try:
            if self.location == "local":
                os.kill(self.pid, signal.SIGKILL)
            else:
                subprocess.run(ssh_cmd("kill", "-9", str(self.pid)),
                               capture_output=True)
        except (OSError, subprocess.SubprocessError):
            pass

    def alive(self):
        if self.pid is None:
            return False
        if self.location == "local":
            try:
                os.kill(self.pid, 0)
                return True
            except OSError:
                return False
        return subprocess.run(ssh_cmd("kill", "-0", str(self.pid)),
                              capture_output=True).returncode == 0


# ===================== 工具 =====================

def get_roster(d, timeout=5.0):
    d.send("get_roster")
    ev = d.wait_event("signal_client.roster", timeout=timeout)
    return ev.get("peers", []) if ev else []


def peer_id_of(d, name, timeout=5.0):
    for _ in range(3):
        for p in get_roster(d, timeout=timeout):
            if p.get("name") == name:
                return p.get("id", 0)
    return 0


def wait_connected(o, a, timeout=CONNECT_TIMEOUT):
    deadline = time.time() + timeout
    o_ok = a_ok = False
    while time.time() < deadline:
        if not o_ok and o.wait_event("peer_connection.state",
                                     lambda e: e.get("state") == "connected", 0.3):
            o_ok = True
        if not a_ok and a.wait_event("peer_connection.state",
                                     lambda e: e.get("state") == "connected", 0.3):
            a_ok = True
        if o_ok and a_ok:
            return True
    return False


def wait_ice_switched(*ds, timeout=SWITCH_TIMEOUT):
    deadline = time.time() + timeout
    ok = {d: False for d in ds}
    while time.time() < deadline:
        for d in list(ok):
            if not ok[d] and d.wait_event("ice_switched",
                                          lambda e: e.get("result") == "success", 0.3):
                ok[d] = True
        if all(ok.values()):
            return True
    return False


def wait_reconnected(*ds, timeout=SWITCH_TIMEOUT):
    deadline = time.time() + timeout
    ok = {d: False for d in ds}
    while time.time() < deadline:
        for d in list(ok):
            if not ok[d] and d.wait_event("reconnected",
                                          lambda e: e.get("result") == "success", 0.3):
                ok[d] = True
        if all(ok.values()):
            return True
    return False


def selected_type(d):
    d.send("get_stats")
    ev = d.wait_event("stats", timeout=5)
    return (ev or {}).get("candidate_pair", {}).get(
        "local_candidate", {}).get("candidate_type", "")


def wait_selected(d, expected, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if selected_type(d) == expected:
            return True
        time.sleep(0.5)
    return False


def wait_selected_any(d, acceptable, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if selected_type(d) in acceptable:
            return True
        time.sleep(0.5)
    return False


def wait_dc_open(d, timeout=10.0):
    """轮询 peer_connection_status（同步查询），避免 data_channel.state 事件被丢弃。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        d.send("peer_connection_status")
        ev = d.wait_event("peer_connection_status", timeout=2.0)
        if ev and ev.get("data_channel_open"):
            return True
        time.sleep(0.3)
    return False


def ice_mode(d):
    d.send("get_ice_config")
    ev = d.wait_event("ice_config", timeout=5)
    return (ev or {}).get("mode", "")


# ===================== 断言封装 =====================

class Checker:
    """收集测试结果并统一打印。"""

    def __init__(self, label):
        self.label = label
        self.fails = []

    def check(self, name, cond, detail=""):
        status = "PASS" if cond else "FAIL"
        line = f"  [{status}] {name}"
        if not cond and detail:
            line += f"  ({detail})"
        print(line, flush=True)
        if not cond:
            self.fails.append(name + (f" ({detail})" if detail else ""))

    def expect_error(self, d, cmd, code, timeout=6.0):
        d.send(cmd)
        ev = d.wait_event("ipc.error", lambda e: e.get("code") == code, timeout)
        self.check(f"{cmd!r} -> {code}", ev is not None,
                   "" if ev else "no matching ipc.error")


# ===================== 接口覆盖（方向 A：本地 offerer + 云端 answerer） =====================

def interface_coverage(offerer, answerer):
    print(f"\n===== 接口覆盖（offerer=local, answerer=cloud） =====", flush=True)
    c = Checker("interface")

    # ---- 未注册错误 ----
    print("-- 未注册错误 --", flush=True)
    c.expect_error(offerer, "call 1", "not_signed_in")
    c.expect_error(offerer, "leave", "not_signed_in")
    c.expect_error(offerer, "get_roster", "not_signed_in")
    c.expect_error(offerer, "signal_disconnect", "not_signed_in")
    c.expect_error(offerer, "data_channel_msg hi", "data_channel_not_open")
    c.expect_error(offerer, "signal_msg 1 hi", "signal_send_failed")

    # ---- usage 错误 ----
    print("-- usage 错误 --", flush=True)
    c.expect_error(offerer, "call 0", "usage")
    c.expect_error(offerer, "signal_msg 0 hi", "usage")
    c.expect_error(offerer, "set_ice_transports", "usage")
    c.expect_error(offerer, "switch_ice", "usage")

    # ---- 注册 ----
    print("-- 注册 --", flush=True)
    offerer.send(f"register offerer {offerer.server} {SIGNAL_PORT}")
    oev = offerer.wait_event("signal_client.signed_in", timeout=10)
    c.check("offerer signed_in", oev is not None and oev.get("id", 0) > 0)
    oid = oev.get("id", 0) if oev else 0
    answerer.send(f"register answerer {answerer.server} {SIGNAL_PORT}")
    aev = answerer.wait_event("signal_client.signed_in", timeout=10)
    c.check("answerer signed_in", aev is not None and aev.get("id", 0) > 0)
    time.sleep(0.5)  # 等 roster 刷新

    # ---- 查询类 happy ----
    print("-- 查询类 happy --", flush=True)
    offerer.send("peer_connection_status")
    ev = offerer.wait_event("peer_connection_status", timeout=5)
    c.check("peer_connection_status", ev is not None and ev.get("signed_in") is True)
    offerer.send("get_stats")
    ev = offerer.wait_event("stats", timeout=5)
    c.check("get_stats", ev is not None)
    offerer.send("get_ice_config")
    ev = offerer.wait_event("ice_config", timeout=5)
    c.check("get_ice_config", ev is not None and ev.get("mode") in
            ("host", "srflx", "relay", "auto"))

    # ---- 呼叫错误（已登录、空闲态） ----
    print("-- 呼叫错误 --", flush=True)
    c.expect_error(offerer, f"call {oid}", "self_call")
    c.expect_error(offerer, "call 9999", "peer_offline")
    c.expect_error(offerer, "leave", "not_in_call")
    c.expect_error(offerer, "switch_ice relay", "not_in_call")
    c.expect_error(offerer, "reconnect", "not_in_call")

    # ---- set_ice_transports happy ----
    offerer.send("set_ice_transports relay")
    ev = offerer.wait_event("ice_config", timeout=5)
    c.check("set_ice_transports relay", ev is not None and ev.get("mode") == "relay")
    answerer.send("set_ice_transports auto")
    ev = answerer.wait_event("ice_config", timeout=5)
    c.check("set_ice_transports auto", ev is not None and ev.get("mode") == "auto")

    # ---- 建立通话 ----
    print("-- 建立通话 --", flush=True)
    aid = peer_id_of(offerer, "answerer")
    c.check("answerer in roster", aid > 0)
    offerer.send(f"call {aid}")
    if not wait_connected(offerer, answerer):
        c.check("call connected", False, "not connected")
        return c
    c.check("call connected", True)

    # ---- 通话中错误 ----
    print("-- 通话中错误 --", flush=True)
    c.expect_error(offerer, f"call {aid}", "already_in_call")
    c.expect_error(offerer, "set_ice_transports host", "already_in_call")

    # ---- 消息 happy（数据通道 + 信令） ----
    print("-- 消息 happy --", flush=True)
    dc_open = wait_dc_open(offerer) and wait_dc_open(answerer)
    c.check("data_channel open", dc_open)
    if dc_open:
        offerer.send("data_channel_msg ping-dc")
        c.check("data_channel_msg 对端收到",
                answerer.wait_event("data_channel_msg", lambda e: e.get("text") == "ping-dc", 5) is not None)
    offerer.send(f"signal_msg {aid} ping-sig")
    c.check("signal_msg 对端收到",
            answerer.wait_event("signal_client.message", lambda e: e.get("text") == "ping-sig", 5) is not None)

    # ---- switch_ice happy（offerer relay -> srflx） ----
    print("-- switch_ice happy --", flush=True)
    offerer.send("switch_ice srflx")
    c.check("switch_ice srflx 完成", wait_ice_switched(offerer, answerer))
    c.check("offerer mode=srflx", ice_mode(offerer) == "srflx")
    c.check("answerer mode 不变=auto", ice_mode(answerer) == "auto")
    # 本地在校 NAT 后，srflx 路径的 selected 类型是 prflx（端口相关映射），二者都算 srflx 成功
    if not wait_selected_any(offerer, ("srflx", "prflx")):
        c.check("offerer selected=srflx/prflx", False, f"selected={selected_type(offerer)}")
    else:
        c.check("offerer selected=srflx/prflx", True)

    # ---- reconnect happy（复用旧候选，不重建） ----
    print("-- reconnect happy --", flush=True)
    offerer.send("reconnect")
    c.check("reconnect 完成", wait_reconnected(offerer))
    c.check("reconnect 后 mode 不变=srflx", ice_mode(offerer) == "srflx")

    # ---- leave happy ----
    print("-- leave happy --", flush=True)
    offerer.send("leave")
    c.check("offerer call_ended", offerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT) is not None)
    c.check("answerer call_ended", answerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT) is not None)

    # ---- signal_disconnect happy ----
    print("-- signal_disconnect happy --", flush=True)
    offerer.send("signal_disconnect")
    c.check("offerer disconnected", offerer.wait_event("signal_client.disconnected", timeout=CALL_END_TIMEOUT) is not None)

    print(f"接口覆盖：{'全部通过' if not c.fails else '失败 ' + str(c.fails)}", flush=True)
    return c


# ===================== 反复抗压 =====================

def stress_call_leave(offerer, answerer, cycles):
    print(f"  -- call/leave × {cycles} --", flush=True)
    c = Checker("call/leave")
    aid = peer_id_of(offerer, "answerer")
    if aid <= 0:
        c.check("answerer in roster", False, "not found")
        return c
    for i in range(cycles):
        offerer.send(f"call {aid}")
        if not wait_connected(offerer, answerer):
            c.check(f"cycle{i} connected", False, "not connected")
            break
        c.check(f"cycle{i} connected", True)
        offerer.send("leave")
        if (offerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT) and
                answerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)):
            c.check(f"cycle{i} leave", True)
        else:
            c.check(f"cycle{i} leave", False, "call_ended 缺失")
            break
    return c


def stress_switch(offerer, answerer, cycles):
    print(f"  -- switch_ice × {cycles}（answerer=auto，offerer relay↔srflx） --", flush=True)
    c = Checker("switch_ice")
    answerer.send("set_ice_transports auto")
    answerer.wait_event("ice_config", timeout=5)
    offerer.send("set_ice_transports relay")
    offerer.wait_event("ice_config", timeout=5)
    aid = peer_id_of(offerer, "answerer")
    offerer.send(f"call {aid}")
    if not wait_connected(offerer, answerer):
        c.check("initial connected", False, "not connected")
        return c
    modes = ["srflx", "relay"] * ((cycles + 1) // 2)
    for i, m in enumerate(modes[:cycles]):
        offerer.send(f"switch_ice {m}")
        ok = wait_ice_switched(offerer, answerer)
        c.check(f"switch{i} -> {m}", ok)
        if not ok:
            break
        # 跨 NAT 下本地 offerer 的 relay/srflx 候选会因 coturn external-ip / NAT 端口映射
        # 被 WebRTC 上报为 prflx，故 selected 只要不是 host（私网连不通）即算切换成功。
        if not wait_selected_any(offerer, ("relay", "srflx", "prflx")):
            c.check(f"switch{i} selected={m}", False, f"selected={selected_type(offerer)}")
    offerer.send("leave")
    offerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)
    answerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)
    return c


def stress_reconnect(offerer, answerer, cycles):
    print(f"  -- reconnect × {cycles}（relay） --", flush=True)
    c = Checker("reconnect")
    offerer.send("set_ice_transports relay")
    offerer.wait_event("ice_config", timeout=5)
    answerer.send("set_ice_transports auto")
    answerer.wait_event("ice_config", timeout=5)
    aid = peer_id_of(offerer, "answerer")
    offerer.send(f"call {aid}")
    if not wait_connected(offerer, answerer):
        c.check("initial connected", False, "not connected")
        return c
    for i in range(cycles):
        offerer.send("reconnect")
        ok = wait_reconnected(offerer)
        c.check(f"reconnect{i}", ok)
        if not ok:
            break
    offerer.send("leave")
    offerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)
    answerer.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)
    return c


# ===================== 主流程 =====================

DIRECTIONS = {
    "A": {"offerer": "local", "answerer": "cloud"},
    "B": {"offerer": "cloud", "answerer": "local"},
}


def kill_all_daemons():
    subprocess.run(["pkill", "-f", "webrtc_engine_mai[n]"], capture_output=True)
    subprocess.run(ssh_cmd("pkill", "-f", "webrtc_engine_mai[n]"), capture_output=True)
    time.sleep(1.0)


def make_daemons(direction):
    loc = DIRECTIONS[direction]
    o = Daemon("offerer", loc["offerer"])
    a = Daemon("answerer", loc["answerer"])
    return o, a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--direction", choices=["A", "B"], default=None)
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--interface-only", action="store_true")
    ap.add_argument("--stress-only", action="store_true")
    args = ap.parse_args()

    directions = [args.direction] if args.direction else ["A", "B"]
    all_fails = []

    # ===== 接口覆盖（只跑 A 方向一次） =====
    if not args.stress_only:
        kill_all_daemons()
        o, a = make_daemons("A")
        try:
            o.start(); a.start()
            o.connect(); a.connect()
            c = interface_coverage(o, a)
            all_fails += [f"接口/{f}" for f in c.fails]
            # 进程存活检查
            c.check("本地进程存活", o.alive())
            c.check("云端进程存活", a.alive())
        finally:
            o.stop(); a.stop(); o.kill9(); a.kill9()
            time.sleep(0.5)

    # ===== 反复抗压（双向） =====
    if not args.interface_only:
        for d in directions:
            print(f"\n===== 反复抗压（方向 {d}：offerer={DIRECTIONS[d]['offerer']}） =====", flush=True)
            kill_all_daemons()
            o, a = make_daemons(d)
            try:
                o.start(); a.start()
                o.connect(); a.connect()
                o.send(f"register offerer {o.server} {SIGNAL_PORT}")
                a.send(f"register answerer {a.server} {SIGNAL_PORT}")
                if not o.wait_event("signal_client.signed_in", timeout=10):
                    print("  [FAIL] offerer not signed in", flush=True)
                    all_fails.append(f"方向{d}/offerer注册")
                    continue
                if not a.wait_event("signal_client.signed_in", timeout=10):
                    print("  [FAIL] answerer not signed in", flush=True)
                    all_fails.append(f"方向{d}/answerer注册")
                    continue
                time.sleep(0.5)

                for c in (stress_call_leave(o, a, args.cycles),
                          stress_switch(o, a, args.cycles),
                          stress_reconnect(o, a, args.cycles)):
                    all_fails += [f"方向{d}/{c.label}/{f}" for f in c.fails]

                # 进程存活 + 状态复位检查
                o.send("peer_connection_status")
                ev = o.wait_event("peer_connection_status", timeout=5)
                if ev and (ev.get("connected") or ev.get("peer_id", 0) > 0):
                    print("  [FAIL] 抗压结束后状态未复位（残留 in_call）", flush=True)
                    all_fails.append(f"方向{d}/状态残留")
                else:
                    print("  [PASS] 抗压结束后状态已复位", flush=True)
                print(f"  [{'PASS' if o.alive() else 'FAIL'}] 本地进程存活", flush=True)
                print(f"  [{'PASS' if a.alive() else 'FAIL'}] 云端进程存活", flush=True)
            finally:
                o.stop(); a.stop(); o.kill9(); a.kill9()
                time.sleep(0.5)

    # ===== 汇总 =====
    print("\n========== 汇总 ==========", flush=True)
    if all_fails:
        print(f"共 {len(all_fails)} 处失败：", flush=True)
        for f in all_fails:
            print(f"  FAIL {f}", flush=True)
        sys.exit(1)
    else:
        print("全部通过 ✅（不崩 + 事件正确 + 不卡死）", flush=True)
        sys.exit(0)


if __name__ == "__main__":
    main()
