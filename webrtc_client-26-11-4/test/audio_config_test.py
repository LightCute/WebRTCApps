#!/usr/bin/env python3
"""音频配置测试（第二期 Task 1）：get_audio_config / set_audio_config 全量 APM 参数。

两个本地 daemon + 云端信令。断言：
  - 默认配置回显（aec 关、ns 开/High、agc 开/adaptive_analog、hpf 开、瞬态关）
  - 逐项设置回显（ns level、agc mode、hpf、瞬态、前置增益字段等）
  - AGC1/AGC2 冲突自动裁决（开 AGC2 关 AGC1）
  - 非法 key / 非法值 → ipc.error usage
  - 设置期间无重协商，音频流不断
  - 旧 set_audio_processing 别名仍可用
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
LOG_DIR = "/tmp/wc_audiocfg_logs"


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
            stderr=open(f"/tmp/wc_audiocfg_stderr_{self.role}.log", "w"))

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


def drop_event(d, name):
    """丢弃缓冲区里旧的同类事件，避免读到上一次操作的回显。"""
    while d.wait(name, timeout=0.01) is not None:
        pass


def get_config(d, timeout=5):
    drop_event(d, "audio_config")
    d.send("get_audio_config")
    ev = d.wait("audio_config", timeout=timeout)
    if ev is None:
        fail(f"{d.role}: get_audio_config 无响应")
    return ev


def set_config(d, key, value, timeout=5):
    drop_event(d, "audio_config")
    d.send(f"set_audio_config {key} {value}")
    return d.wait("audio_config", timeout=timeout)


def expect_error(d, code="usage", timeout=5):
    ev = d.wait("ipc.error", timeout=timeout)
    if ev is None:
        fail(f"{d.role}: 期望 ipc.error {code}，未收到")
    if ev.get("code") != code:
        fail(f"{d.role}: 期望 {code}，收到 {ev.get('code')} ({ev.get('msg')})")


def expect_value(cfg, path, expected):
    cur = cfg
    for p in path.split("."):
        if not isinstance(cur, dict) or p not in cur:
            fail(f"配置缺少字段 {path}: {cfg}")
        cur = cur[p]
    if cur != expected:
        fail(f"配置 {path} 期望 {expected}，实际 {cur}")


def audio_streams(d):
    d.send("get_stats")
    st = d.wait("stats", timeout=5)
    if st is None:
        return []
    return [s for s in st.get("inbound_rtp", []) if s.get("kind") == "audio"]


def wait_audio_growing(d, timeout, gap=1.5, min_delta=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        before = {s["ssrc"]: s["packets_received"] for s in audio_streams(d)}
        time.sleep(gap)
        for s in audio_streams(d):
            prev = before.get(s["ssrc"])
            if prev is not None and s["packets_received"] - prev > min_delta:
                return s
    return None


def main():
    offerer = Daemon("offerer", "/tmp/wc_audiocfg_o.sock")
    answerer = Daemon("answerer", "/tmp/wc_audiocfg_a.sock")
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
        answerer.send("set_ice_transports auto")
        answerer.wait("ice_config", timeout=5)

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
            if aid:
                break
            time.sleep(0.5)
        if not aid:
            fail("roster 中找不到 answerer")
        offerer.send(f"call {aid}")
        for d in (offerer, answerer):
            if not d.wait("peer_connection.state",
                          lambda e: e.get("state") == "connected", 20):
                fail(f"{d.role}: 未 connected")
        time.sleep(1)
        if wait_audio_growing(answerer, 10) is None:
            fail("初始音频流未建立")

        # 默认值
        cfg = get_config(offerer)
        expect_value(cfg, "processing.echo_canceller.enabled", False)
        expect_value(cfg, "processing.noise_suppression.enabled", True)
        expect_value(cfg, "processing.noise_suppression.level", "high")
        expect_value(cfg, "processing.gain_controller1.enabled", True)
        expect_value(cfg, "processing.gain_controller1.mode", "adaptive_analog")
        expect_value(cfg, "processing.high_pass_filter.enabled", True)
        expect_value(cfg, "processing.transient_suppression.enabled", False)
        expect_value(cfg, "processing.gain_controller2.enabled", False)
        print("T1 默认配置回显正确")

        offerer.drain()
        answerer.drain()

        # 逐项设置
        cases = [
            ("noise_suppression.level", "very_high",
             "processing.noise_suppression.level", "very_high"),
            ("gain_controller1.mode", "fixed_digital",
             "processing.gain_controller1.mode", "fixed_digital"),
            ("high_pass_filter.enabled", "off",
             "processing.high_pass_filter.enabled", False),
            ("transient_suppression.enabled", "on",
             "processing.transient_suppression.enabled", True),
            ("pre_amplifier.enabled", "on",
             "processing.pre_amplifier.enabled", True),
            ("pre_amplifier.fixed_gain_factor", "2.0",
             "processing.pre_amplifier.fixed_gain_factor", 2.0),
            ("gain_controller1.target_level_dbfs", "6",
             "processing.gain_controller1.target_level_dbfs", 6),
            ("noise_suppression.analyze_linear_aec_output_when_available",
             "on",
             "processing.noise_suppression.analyze_linear_aec_output_when_available",
             True),
        ]
        for key, value, path, expected in cases:
            ev = set_config(offerer, key, value)
            if ev is None:
                fail(f"set_audio_config {key} {value} 无回显")
            expect_value(ev, path, expected)
            print(f"T2 set {key}={value} OK")

        # AGC1/AGC2 冲突：开 AGC2 自动关 AGC1
        ev = set_config(offerer, "gain_controller2.enabled", "on")
        if ev is None:
            fail("开 AGC2 无回显")
        expect_value(ev, "processing.gain_controller2.enabled", True)
        expect_value(ev, "processing.gain_controller1.enabled", False)
        print("T3 开 AGC2 自动关 AGC1")

        # 前置增益与 capture_level_adjustment 冲突：后者开则前者关
        ev = set_config(offerer, "capture_level_adjustment.enabled", "on")
        if ev is None:
            fail("开 capture_level_adjustment 无回显")
        expect_value(ev, "processing.capture_level_adjustment.enabled", True)
        expect_value(ev, "processing.pre_amplifier.enabled", False)
        print("T3 开 capture_level_adjustment 自动关 pre_amplifier")

        # 非法 key / 值
        offerer.send("set_audio_config no.such.key 1")
        expect_error(offerer)
        offerer.send("set_audio_config noise_suppression.level bogus")
        expect_error(offerer)
        offerer.send("set_audio_config gain_controller1.target_level_dbfs 99")
        expect_error(offerer)
        print("T4 非法 key/值 → usage")

        # 旧别名仍可用
        offerer.send("set_audio_processing ns off")
        ev = offerer.wait("audio_processing", timeout=5)
        if ev is None or ev.get("ns") is not False:
            fail(f"旧 set_audio_processing 别名失效: {ev}")
        offerer.send("set_audio_processing ns on")
        offerer.wait("audio_processing", timeout=5)
        print("T5 旧 set_audio_processing 别名可用")

        # 无重协商 + 流不断
        time.sleep(1)
        for d in (offerer, answerer):
            sdp = [e for e in d.events()
                   if e.get("event") == "signal_client.local_sdp"]
            if sdp:
                fail(f"{d.role}: 配置变更触发了重协商: {sdp}")
        if wait_audio_growing(answerer, 10) is None:
            fail("配置变更后音频流中断")
        print("T6 配置变更无重协商、音频流持续")

        # T7 源参数：先 pending，audio_off/on 建轨后生效
        ev = set_config(offerer, "source.stereo_swapping", "on")
        if ev is None:
            fail("设置 source.stereo_swapping 无回显")
        if "source.stereo_swapping" not in ev.get("pending_source", []):
            fail(f"pending_source 未包含 source.stereo_swapping: {ev}")
        expect_value(ev, "source.stereo_swapping", True)

        offerer.send("audio_off")
        if not offerer.wait("audio_switched",
                            lambda e: e.get("result") == "success"
                            and e.get("enabled") is False, 15):
            fail("audio_off（源参数生效前）失败")
        offerer.send("audio_on")
        if not offerer.wait("audio_switched",
                            lambda e: e.get("result") == "success"
                            and e.get("enabled") is True, 15):
            fail("audio_on（源参数生效）失败")
        cfg = get_config(offerer)
        if cfg.get("pending_source") != []:
            fail(f"建轨后 pending_source 未清空: {cfg.get('pending_source')}")
        expect_value(cfg, "source.stereo_swapping", True)
        print("T7 源参数 pending → 建轨后生效")

        # T8 非法源参数
        offerer.send("set_audio_config source.jitter_max_packets -1")
        expect_error(offerer)
        offerer.send("set_audio_config source.nope 1")
        expect_error(offerer)
        print("T8 非法源参数 → usage")

        # T9 设备可用性 + 硬件采样率 + ADM 实际录音采样率
        offerer.send("get_audio_devices")
        ev = offerer.wait("audio_devices", timeout=10)
        if ev is None or not ev.get("devices"):
            fail("get_audio_devices 无设备")
        devices = ev["devices"]
        device_ids = [d["id"] for d in devices]
        if len(set(device_ids)) != len(device_ids):
            fail(f"设备 id 不唯一: {device_ids}")
        for dev in devices:
            if dev.get("available") not in ("yes", "no", "unknown"):
                fail(f"available 字段非法: {dev}")
        hw = [d for d in devices if d.get("hardware_sample_rate", 0) > 0]
        if not hw:
            fail(f"没有设备返回硬件采样率: {devices}")
        real_hw = [d for d in hw
                   if not d["name"].startswith("default:")]
        if not real_hw:
            fail(f"真实设备没有硬件采样率（default 别名不算）: {devices}")
        cfg = get_config(offerer)
        if cfg.get("device", {}).get("recording_sample_rate", 0) <= 0:
            fail(f"recording_sample_rate 未回显: {cfg.get('device')}")
        print(f"T9 可用性/硬件采样率 OK: "
              f"{[(d['name'], d['available'], d['hardware_sample_rate']) for d in hw]}")

        print("audio config test ok")
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
