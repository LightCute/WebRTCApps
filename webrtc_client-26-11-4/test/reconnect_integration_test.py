#!/usr/bin/env python3
"""跨 NAT 断开/重连抗压测试。

驱动真实 daemon（webrtc_engine_main）经 unix socket 走 IPC 协议，反复「建立通话 →
断开 → 重连」，覆盖 2 方向 × 8 项 × 每项 5 遍。

用法：
  python3 reconnect_integration_test.py                      # 全量 2×8×5
  python3 reconnect_integration_test.py --direction A --items offer_leave
  python3 reconnect_integration_test.py --cycles 1           # 冒烟：每项只断连 1 遍

判定（事件驱动）：
  connected = peer_connection.state 事件出现 connected
  断开完成 = signal_client.call_ended 事件（leave / 对端消失）
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
SIGNAL_SERVER = "120.79.210.6"   # 云端信令服务器
SIGNAL_PORT = 8888
LOCAL_SOCK = "/tmp/wc_local.sock"
CLOUD_SOCK = "/tmp/wc_cloud.sock"
LOCAL_ENV = "/home/light/webrtc/checkout/src/apps/webrtc_client-26-11-4/.env"
CLOUD_ENV = "/root/stun_test/.env"

CONNECT_TIMEOUT = 15   # 单次 connected 超时（秒）
CALL_END_TIMEOUT = 10  # 断开完成超时（秒）
RETRY_MAX = 2          # srflx 抖动重试次数


def ssh_cmd(*args):
    return ["ssh", "-o", "StrictHostKeyChecking=no",
            "-o", "ConnectTimeout=10", f"{CLOUD_USER}@{CLOUD_HOST}"] + list(args)


# ===================== Daemon 封装 =====================

class Daemon:
    """一个 webrtc_engine_main 进程 + 其 unix socket 连接。"""

    def __init__(self, role, location):
        self.role = role            # "offerer" / "answerer"
        self.location = location    # "local" / "cloud"
        self.sock = LOCAL_SOCK if location == "local" else CLOUD_SOCK
        self.log_dir = "/tmp/wc_logs_local" if location == "local" else "/tmp/wc_logs_cloud"
        self.server = SIGNAL_SERVER if location == "local" else "127.0.0.1"
        self.env_file = LOCAL_ENV if location == "local" else CLOUD_ENV

        self.pid = None
        self.proc = None            # 本地进程 / 云端 nc 桥进程
        self.rfile = None           # 可读文件（读事件）
        self.wfile = None           # 可写文件（写命令）
        self.q = queue.Queue()
        self.reader = None

    # ---- 启动 daemon 进程 ----
    def start(self):
        args = [f"--socket_path={self.sock}",
                f"--env_file={self.env_file}",
                f"--log_dir={self.log_dir}"]
        if self.location == "local":
            self._rm_sock_local()
            self.proc = subprocess.Popen(
                [LOCAL_BIN] + args,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.pid = self.proc.pid
        else:
            # 云端：清残留 socket，nohup 后台启动，回显 pid
            cmd = (f"rm -f {self.sock}; nohup {CLOUD_BIN} " +
                   " ".join(args) +
                   f" > /tmp/wc_cloud_daemon.log 2>&1 & echo $!")
            out = subprocess.check_output(ssh_cmd(cmd), text=True).strip()
            self.pid = int(out.splitlines()[-1])

    def _rm_sock_local(self):
        try:
            if os.path.exists(self.sock):
                os.unlink(self.sock)
        except OSError:
            pass

    # ---- 连接 socket ----
    def connect(self):
        if self.location == "local":
            for _ in range(40):  # 等 socket 文件出现
                if os.path.exists(self.sock):
                    break
                time.sleep(0.25)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(self.sock)
            f = s.makefile("rw", buffering=1)
            self.rfile = f
            self.wfile = f
        else:
            # 云端：ssh + nc -U 桥接（stdin 写命令 / stdout 读事件）
            for _ in range(40):
                if self._cloud_sock_exists():
                    break
                time.sleep(0.25)
            self.proc = subprocess.Popen(
                ssh_cmd("nc", "-U", self.sock),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, bufsize=1)
            self.rfile = self.proc.stdout
            self.wfile = self.proc.stdin

        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def _cloud_sock_exists(self):
        out = subprocess.run(ssh_cmd(f"test -S {self.sock}"),
                             capture_output=True).returncode == 0
        return out

    # ---- 后台读事件 ----
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
            pass  # 连接关闭

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

    # ---- 停止 / 强杀 ----
    def stop(self):
        try:
            self.send("quit")
        except Exception:
            pass
        time.sleep(0.5)

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


# ===================== 工具 =====================

def get_roster(d, timeout=5.0):
    """请求 roster 并返回 peers 列表 [{id,name,state}]。"""
    d.send("get_roster")
    ev = d.wait_event("signal_client.roster", timeout=timeout)
    return ev.get("peers", []) if ev else []


def peer_id_of(d, name, timeout=5.0):
    for _ in range(3):
        for p in get_roster(d, timeout=timeout):
            if p.get("name") == name:
                return p.get("id", 0)
    return 0


def wait_connected(offerer, answerer, timeout=CONNECT_TIMEOUT):
    deadline = time.time() + timeout
    o_ok = a_ok = False
    while time.time() < deadline:
        if not o_ok:
            ev = offerer.wait_event(
                "peer_connection.state",
                lambda e: e.get("state") == "connected", 0.3)
            if ev:
                o_ok = True
        if not a_ok:
            ev = answerer.wait_event(
                "peer_connection.state",
                lambda e: e.get("state") == "connected", 0.3)
            if ev:
                a_ok = True
        if o_ok and a_ok:
            return True
    return False


# ===================== 8 项定义 =====================

# teardown: leave | disconnect | quit | sigkill
ITEMS = [
    ("offer_leave",     "offerer", "leave"),
    ("answer_leave",    "answerer", "leave"),
    ("offer_disconnect","offerer", "disconnect"),
    ("answer_disconnect","answerer", "disconnect"),
    ("offer_quit",      "offerer", "quit"),
    ("answer_quit",     "answerer", "quit"),
    ("offer_sigkill",   "offerer", "sigkill"),
    ("answer_sigkill",  "answerer", "sigkill"),
]

# direction: A = offerer 在本地；B = offerer 在云端
DIRECTIONS = {
    "A": {"offerer": "local", "answerer": "cloud"},
    "B": {"offerer": "cloud", "answerer": "local"},
}


# ===================== 单条项的执行 =====================

def run_item(direction, item, cycles):
    """跑一个 (方向, 项)：1 次初始建立 + cycles 次断连。返回通过/失败。"""
    name, initiator_role, teardown = item
    loc = DIRECTIONS[direction]
    offerer = Daemon("offerer", loc["offerer"])
    answerer = Daemon("answerer", loc["answerer"])

    result = {"item": name, "direction": direction,
              "connected": 0, "teardowns": 0, "fail": ""}

    try:
        offerer.start(); answerer.start()
        offerer.connect(); answerer.connect()
        offerer.send(f"register offerer {offerer.server} {SIGNAL_PORT}")
        answerer.send(f"register answerer {answerer.server} {SIGNAL_PORT}")

        # 等双方 signed_in
        if not offerer.wait_event("signal_client.signed_in", timeout=10):
            result["fail"] = "offerer not signed in"
            return result
        if not answerer.wait_event("signal_client.signed_in", timeout=10):
            result["fail"] = "answerer not signed in"
            return result

        initiator = offerer if initiator_role == "offerer" else answerer
        survivor = answerer if initiator_role == "offerer" else offerer

        # 每遍：断连一次；共 cycles 次断连 => cycles+1 次 connected
        for cycle in range(cycles + 1):
            # ---- 建立（带 srflx 抖动重试）----
            connected = False
            for attempt in range(RETRY_MAX + 1):
                aid = peer_id_of(offerer, "answerer")
                if aid <= 0:
                    result["fail"] = f"cycle{cycle}: answerer not in roster"
                    return result
                offerer.send(f"call {aid}")
                if wait_connected(offerer, answerer):
                    connected = True
                    break
                # 失败：干净复位后重试
                offerer.send("leave"); answerer.send("leave")
                time.sleep(1.0)
            if not connected:
                result["fail"] = f"cycle{cycle}: connect failed after retries"
                return result
            result["connected"] += 1

            # 最后一遍建立后不再断开
            if cycle == cycles:
                break

            # ---- 断开 ----
            if teardown == "leave":
                initiator.send("leave")
                survivor.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)
                initiator.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)

            elif teardown == "disconnect":
                initiator.send("signal_disconnect")
                initiator.wait_event("signal_client.disconnected", timeout=CALL_END_TIMEOUT)
                survivor.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)
                time.sleep(0.8)  # 等服务器释放名字
                initiator.send(f"register {initiator.role} {initiator.server} {SIGNAL_PORT}")
                initiator.wait_event("signal_client.signed_in", timeout=10)

            elif teardown in ("quit", "sigkill"):
                # 断开方进程退出；存活方收到 peer_gone -> call_ended
                if teardown == "quit":
                    initiator.send("quit")
                else:
                    initiator.kill9()
                time.sleep(1.0)
                survivor.wait_event("signal_client.call_ended", timeout=CALL_END_TIMEOUT)
                # 重启断开方
                initiator.start()
                initiator.connect()
                initiator.send(f"register {initiator.role} {initiator.server} {SIGNAL_PORT}")
                if not initiator.wait_event("signal_client.signed_in", timeout=10):
                    result["fail"] = f"cycle{cycle}: restarted {initiator.role} not signed in"
                    return result

            result["teardowns"] += 1

    finally:
        offerer.stop(); answerer.stop()
        time.sleep(0.5)

    return result


# ===================== 主流程 =====================

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--direction", choices=["A", "B"], default=None,
                    help="只跑某个方向（默认 A、B 都跑）")
    ap.add_argument("--items", default=None,
                    help="逗号分隔的项名，如 offer_leave,answer_leave（默认全部 8 项）")
    ap.add_argument("--cycles", type=int, default=5,
                    help="每项断连次数（默认 5）")
    args = ap.parse_args()

    directions = [args.direction] if args.direction else ["A", "B"]
    if args.items:
        wanted = set(args.items.split(","))
        items = [it for it in ITEMS if it[0] in wanted]
    else:
        items = ITEMS

    results = []
    t0 = time.time()
    for d in directions:
        for item in items:
            r = run_item(d, item, args.cycles)
            results.append(r)
            status = "PASS" if not r["fail"] else "FAIL"
            print(f"[{status}] {d} {r['item']}: "
                  f"connected={r['connected']} teardowns={r['teardowns']}"
                  + (f" fail={r['fail']}" if r["fail"] else ""),
                  flush=True)

    failed = [r for r in results if r["fail"]]
    print("\n========== 汇总 ==========")
    print(f"共 {len(results)} 项，通过 {len(results)-len(failed)}，失败 {len(failed)}，"
          f"用时 {time.time()-t0:.0f}s")
    for r in failed:
        print(f"  FAIL {r['direction']} {r['item']}: {r['fail']}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
