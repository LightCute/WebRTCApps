#!/usr/bin/env python3
"""音频第一期测试：设备枚举/选择 + 空闲/通话中开关 + 错误路径。

两个本地 daemon（PulseAudio 共享麦克风）+ 云端信令 120.79.210.6:8888。
验证：
  A5 get_audio_devices 枚举（monitor 伪设备已过滤）
  A6 空闲 set_audio_device 生效 + get_audio_status 回显
  A1 空闲 audio_on + call → 对端 inbound_rtp 出现 audio/opus 且包数递增
  A2 audio_off → 对端停止收流；A3 audio_on 恢复
  A7 通话中 set_audio_device → unsupported_in_call
  A8 非法设备 / 非法处理参数 → device_not_found / usage
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
LOG_DIR = "/tmp/wc_audio_logs"


class Daemon:
    def __init__(self, role, sock):
        self.role = role
        self.sock = sock
        self.proc = None
        self.rfile = None
        self.wfile = None
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
            stderr=open(f"/tmp/wc_audio_stderr_{self.role}.log", "w"))

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
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            for line in self.rfile:
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
        """等待事件；不匹配的事件保留在 buf 里，供后续 wait 使用。"""
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

    def events(self):
        with self.cv:
            return list(self.buf)

    def drain(self):
        with self.cv:
            self.buf.clear()

    def drop(self, event):
        """只丢掉指定事件，保留其他未匹配事件。"""
        with self.cv:
            self.buf[:] = [e for e in self.buf if e.get("event") != event]


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def audio_streams(d):
    d.send("get_stats")
    st = d.wait("stats", timeout=5)
    if st is None:
        fail(f"{d.role}: get_stats 无响应")
    return [s for s in st.get("inbound_rtp", []) if s.get("kind") == "audio"]


def wait_audio_growing(d, timeout, gap=1.5, min_delta=5, exclude_ssrcs=()):
    """轮询直到某路音频在 gap 秒内包数增长；返回该流，超时返回 None。

    媒体重建后旧流会残留在 stats 里且停止增长，所以按 SSRC 逐路判断，
    不能只取第一路。
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        before = {s["ssrc"]: s["packets_received"] for s in audio_streams(d)}
        time.sleep(gap)
        for s in audio_streams(d):
            prev = before.get(s["ssrc"])
            if prev is None or s["ssrc"] in exclude_ssrcs:
                continue
            if s["packets_received"] - prev > min_delta:
                return s
    return None


def expect_error(d, code):
    ev = d.wait("ipc.error", timeout=5)
    if ev is None:
        fail(f"{d.role}: 期望 ipc.error {code}，未收到")
    if ev.get("code") != code:
        fail(f"{d.role}: 期望 {code}，收到 {ev.get('code')} ({ev.get('msg')})")
    return ev


def wait_connected(d, timeout=20):
    if not d.wait("peer_connection.state",
                  lambda e: e.get("state") == "connected", timeout):
        fail(f"{d.role}: 未 connected")


def main():
    offerer = Daemon("offerer", "/tmp/wc_audio_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_audio_a.sock")
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
        # answerer 用 auto（含 host）：后面 switch_ice host 时才连得通；
        # 纯 relay 对端在云端，够不到本机私网 host 候选。
        answerer.send("set_ice_transports auto")
        answerer.wait("ice_config", timeout=5)

        # A5: 设备枚举
        offerer.send("get_audio_devices")
        ev = offerer.wait("audio_devices", timeout=10)
        if ev is None:
            fail("audio_devices 无响应")
        devices = ev.get("devices", [])
        if not devices:
            fail("音频设备列表为空（PulseAudio 是否可用？看 daemon stderr）")
        for dev in devices:
            if dev.get("id", "").endswith(".monitor"):
                fail(f"枚举出 monitor 伪设备: {dev}")
            if not dev.get("id"):
                fail(f"设备缺少稳定 id: {dev}")
        print(f"A5 枚举到 {len(devices)} 个录音设备: "
              f"{[d['name'] for d in devices]}")

        # A8: 非法设备
        offerer.send("set_audio_device no.such.device")
        expect_error(offerer, "device_not_found")
        print("A8 非法设备 → device_not_found")

        # A6: 空闲选设备（优先选独立 USB 麦克风）
        preferred = next(
            (d for d in devices
             if not d["name"].startswith("default:")
             and ("USB MIC" in d["name"] or "Jieli" in d["name"])),
            devices[0])
        offerer.send(f"set_audio_device {preferred['id']}")
        ev = offerer.wait("audio_device", timeout=5)
        if ev is None or ev.get("result") != "success":
            fail(f"set_audio_device 失败: {ev}")
        offerer.send("get_audio_status")
        st = offerer.wait("audio_status", timeout=5)
        if st is None or st.get("device_id") != preferred["id"]:
            fail(f"audio_status device_id 回显错误: {st}")
        if not st.get("has_device_module"):
            fail("平台 ADM 不可用（已回退 dummy），请检查 PulseAudio")
        print(f"A6 选中设备: {preferred['name']}")

        # A8: 非法处理参数
        offerer.send("set_audio_processing bogus on")
        expect_error(offerer, "usage")
        offerer.send("set_audio_processing aec maybe")
        expect_error(offerer, "usage")
        print("A8 非法处理参数 → usage")

        # A1: 空闲 audio_on（记住），再 call
        for d in (offerer, answerer):
            d.send("audio_on")
            if not d.wait("audio.state", lambda e: e.get("enabled") is True, 5):
                fail(f"{d.role}: audio.state(true) 未收到")
            if not d.wait("audio_switched",
                          lambda e: e.get("result") == "success", 5):
                fail(f"{d.role}: audio_switched(success) 未收到")
        print("A1 空闲 audio_on 成功（双方）")

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
        wait_connected(offerer)
        wait_connected(answerer)
        time.sleep(1.5)

        o = wait_audio_growing(offerer, timeout=10)
        a = wait_audio_growing(answerer, timeout=10)
        if o is None or "opus" not in (o.get("codec") or ""):
            fail(f"offerer 音频流未确认: {o}")
        if a is None or "opus" not in (a.get("codec") or ""):
            fail(f"answerer 音频流未确认: {a}")
        print(f"A1 双向音频流流动: offerer ssrc={o['ssrc']} / "
              f"answerer ssrc={a['ssrc']} (audio/opus)")

        # A7: 通话中切设备（第二期支持，不重协商；允许短暂采集中断）
        other = next((d for d in devices
                      if d["name"] != preferred["name"]
                      and not d["name"].startswith("default:")), None)
        if other is not None:
            offerer.send(f"set_audio_device {other['name']}")
            ev = offerer.wait("audio_device", timeout=15)
            if ev is None or ev.get("result") != "success":
                fail(f"A7 通话中切设备失败: {ev}")
            if wait_audio_growing(answerer, 15) is None:
                fail("A7 切设备后对端收流未恢复")
            print(f"A7 通话中切设备 OK: {other['name']}")
        else:
            offerer.send("set_audio_device no.such.device")
            expect_error(offerer, "device_not_found")
            print("A7 只有单个设备，验证非法设备路径")

        # A2: audio_off（offerer 停止发送）
        offerer.send("audio_off")
        if not offerer.wait("audio_switched",
                            lambda e: e.get("result") == "success"
                            and e.get("enabled") is False, 15):
            fail("offerer audio_off 未完成")
        time.sleep(1)
        still = wait_audio_growing(answerer, timeout=2)
        if still is not None:
            fail(f"audio_off 后 answerer 仍在收音频: {still}")
        print("A2 audio_off 生效（answerer 收流停止）")

        # A3: audio_on 恢复
        old_ssrcs = {s["ssrc"] for s in audio_streams(answerer)}
        offerer.send("audio_on")
        if not offerer.wait("audio_switched",
                            lambda e: e.get("result") == "success"
                            and e.get("enabled") is True, 15):
            fail("offerer audio_on 恢复未完成")
        # 重新加轨会产生新 SSRC；旧流会残留在 stats 里，排除后等新流增长。
        a4 = wait_audio_growing(answerer, timeout=25,
                                exclude_ssrcs=old_ssrcs)
        if a4 is None:
            fail("audio_on 后 answerer 收流未恢复")
        print(f"A3 audio_on 恢复: answerer ssrc={a4['ssrc']} "
              f"packets={a4['packets_received']}")

        # A3b: 对齐视频 stress_combo A1 —— 通话中 switch_ice host/relay，
        # 每次重建 PC 后音频轨应自动补回（RebuildAndOffer / OnSignalReoffer 路径）。
        # 依赖 WebRTC Pulse ADM 补丁（audio_device_pulse_linux.cc 的 Init() 复位
        # quit_）；本机双 daemon 共享 Pulse 设备，重建时设备激活较慢，故等 45s。
        for mode in ("host", "relay"):
            offerer.drop("ice_switched")
            answerer.drop("ice_switched")
            offerer.drop("peer_connection.state")
            answerer.drop("peer_connection.state")
            offerer.send(f"switch_ice {mode}")
            if not offerer.wait("ice_switched",
                                lambda e: e.get("result") == "success", 45):
                fail(f"A3b switch_ice {mode}: 命令端未成功")
            if not answerer.wait("ice_switched",
                                 lambda e: e.get("result") == "success", 45):
                fail(f"A3b switch_ice {mode}: 对端未成功")
            wait_connected(offerer)
            wait_connected(answerer)
            b = wait_audio_growing(answerer, timeout=20)
            if b is None:
                fail(f"A3b switch_ice {mode} 后音频未恢复")
            print(f"A3b switch_ice {mode} 重建后音频自动恢复: "
                  f"ssrc={b['ssrc']} packets={b['packets_received']}")

        print("audio test ok")
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
