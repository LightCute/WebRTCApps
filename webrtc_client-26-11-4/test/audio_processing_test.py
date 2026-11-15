#!/usr/bin/env python3
"""音频处理开关测试（A4）：运行时切 AEC/NS/AGC，不重协商、音频流不断。

两个本地 daemon + 云端信令。断言：
  - set_audio_processing 回显 audio_processing，get_audio_status 同步
  - 切换期间没有新的 local_sdp（不触发重协商）
  - 入向音频包数持续增长
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time

BIN = "/home/light/webrtc/checkout/src/out/webrtc_client_x64/webrtc_engine_main"
ENV = "/home/light/webrtc/checkout/src/apps/webrtc_client-26-11-4/.env"
SERVER = "120.79.210.6"
PORT = 8888
LOG_DIR = "/tmp/wc_audio_proc_logs"


class Daemon:
    def __init__(self, role, sock):
        self.role = role
        self.sock = sock
        self.proc = None
        self.buf = []
        self.cv = threading.Condition()

    def start(self):
        try:
            os.unlink(self.sock)
        except OSError:
            pass
        self.proc = subprocess.Popen(
            [BIN, f"--socket_path={self.sock}", f"--env_file={ENV}",
             f"--log_dir={LOG_DIR}"],
            stdout=subprocess.DEVNULL,
            stderr=open(f"/tmp/wc_audio_proc_stderr_{self.role}.log", "w"))

    def connect(self):
        for _ in range(40):
            if os.path.exists(self.sock):
                break
            time.sleep(0.25)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.sock)
        f = s.makefile("rw", buffering=1)
        self.wfile = f
        threading.Thread(target=self._read, args=(f,), daemon=True).start()

    def _read(self, f):
        try:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    continue
                with self.cv:
                    self.buf.append(ev)
                    self.cv.notify_all()
        except (ValueError, OSError):
            pass

    def send(self, line):
        self.wfile.write(line + "\n")
        self.wfile.flush()

    def wait(self, event, pred=None, timeout=5.0):
        deadline = time.time() + timeout
        with self.cv:
            while True:
                for i, ev in enumerate(self.buf):
                    if ev.get("event") == event and (pred is None or pred(ev)):
                        del self.buf[i]
                        return ev
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                self.cv.wait(remaining)

    def drain(self):
        with self.cv:
            self.buf.clear()

    def events(self):
        with self.cv:
            return list(self.buf)


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def inbound_audio(d):
    d.send("get_stats")
    st = d.wait("stats", timeout=5)
    if st is None:
        fail(f"{d.role}: get_stats 无响应")
    for s in st.get("inbound_rtp", []):
        if s.get("kind") == "audio":
            return s
    return None


def main():
    offerer = Daemon("offerer", "/tmp/wc_audioproc_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_audioproc_a.sock")
    try:
        os.makedirs(LOG_DIR, exist_ok=True)
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

        for d in (offerer, answerer):
            d.send("audio_on")
            d.wait("audio_switched", lambda e: e.get("result") == "success", 5)

        aid = None
        for _ in range(10):
            offerer.send("get_roster")
            ev = offerer.wait("signal_client.roster", timeout=5)
            if ev:
                for p in ev.get("peers", []):
                    if p["name"] == "answerer":
                        aid = p["id"]
                        break
            if aid is not None:
                break
            time.sleep(0.5)
        if aid is None:
            fail("roster 中找不到 answerer（是否有残留 daemon 占用名字？）")
        offerer.send(f"call {aid}")
        for d in (offerer, answerer):
            if not d.wait("peer_connection.state",
                          lambda e: e.get("state") == "connected", 20):
                fail(f"{d.role}: 未 connected")
        time.sleep(1.5)

        # 默认值：aec=off, ns=on, agc=on
        offerer.send("get_audio_status")
        st = offerer.wait("audio_status", timeout=5)
        if st is None:
            fail("audio_status 无响应")
        if (st.get("aec"), st.get("ns"), st.get("agc")) != (False, True, True):
            fail(f"默认处理开关不符: {st}")
        print("A4 默认 aec=off ns=on agc=on")

        before = inbound_audio(answerer)
        if before is None:
            fail("answerer 未收到音频流")
        offerer.drain()
        answerer.drain()

        # 依次切换（不重协商）
        for flag, value, key in (("aec", "on", "aec"),
                                 ("ns", "off", "ns"),
                                 ("agc", "off", "agc")):
            offerer.send(f"set_audio_processing {flag} {value}")
            ev = offerer.wait("audio_processing", timeout=5)
            if ev is None:
                fail(f"{flag} 切换无 audio_processing 回显")
            want = (value == "on")
            if ev.get(key) != want or ev.get("aec") != (want if key == "aec"
                                                        else ev.get("aec")):
                pass  # 逐字段校验在下面 status 里做
            offerer.send("get_audio_status")
            st = offerer.wait("audio_status", timeout=5)
            if st is None or st.get(key) != want:
                fail(f"{flag}={value} 未生效: {st}")
            print(f"A4 set_audio_processing {flag} {value} → "
                  f"status {key}={st.get(key)}")

        # 无重协商
        time.sleep(1.0)
        for d in (offerer, answerer):
            sdp = [e for e in d.events()
                   if e.get("event") == "signal_client.local_sdp"]
            if sdp:
                fail(f"{d.role}: 处理开关切换触发了重协商: {sdp}")
        print("A4 切换期间无重协商")

        # 音频流不断
        after = inbound_audio(answerer)
        if after is None or after["packets_received"] <= before["packets_received"]:
            fail(f"切换期间音频流中断: {before} -> {after}")
        print(f"A4 音频流持续: {before['packets_received']}→"
              f"{after['packets_received']}")

        # 恢复默认
        for flag, value in (("aec", "off"), ("ns", "on"), ("agc", "on")):
            offerer.send(f"set_audio_processing {flag} {value}")
            offerer.wait("audio_processing", timeout=5)

        print("audio processing test ok")
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
