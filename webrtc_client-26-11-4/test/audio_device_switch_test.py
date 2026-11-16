#!/usr/bin/env python3
"""通话中切音频输入设备测试（第二期 Task 4）。

断言：
  - 空闲 set_audio_device 生效并回显
  - 通话中 set_audio_device 成功，对端收流短暂停滞后恢复
  - 通话中切非法设备 → device_not_found，原设备继续收流
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_config_test import (  # noqa: E402
    Daemon,
    SERVER,
    PORT,
    fail,
    get_config,
    wait_audio_growing,
)


def get_devices(d):
    d.send("get_audio_devices")
    ev = d.wait("audio_devices", timeout=10)
    if ev is None:
        fail(f"{d.role}: get_audio_devices 无响应")
    if not ev.get("devices"):
        fail("音频设备列表为空（PulseAudio 不可用？）")
    return ev["devices"]


def set_device(d, name, timeout=15):
    d.send(f"set_audio_device {name}")
    return d.wait("audio_device", timeout=timeout)


def main():
    offerer = Daemon("sw_o", "/tmp/wc_sw_o.sock")
    answerer = Daemon("sw_a", "/tmp/wc_sw_a.sock")
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        offerer.send(f"register sw_o {SERVER} {PORT}")
        answerer.send(f"register sw_a {SERVER} {PORT}")
        if not offerer.wait("signal_client.signed_in", timeout=10):
            fail("offerer not signed in")
        if not answerer.wait("signal_client.signed_in", timeout=10):
            fail("answerer not signed in")
        answerer.send("set_ice_transports auto")
        answerer.wait("ice_config", timeout=5)

        devices = get_devices(offerer)
        real = [d for d in devices if not d["name"].startswith("default:")]
        # ADM 对同名设备（两个同型号摄像头麦）无法区分，测试用不同名字的设备。
        unique = []
        for d in real:
            if d["name"] not in [u["name"] for u in unique]:
                unique.append(d)
        if len(unique) < 2:
            fail(f"需要至少 2 个不同名的录音设备，实际: "
                 f"{[d['name'] for d in devices]}")
        first = unique[0]
        second = unique[1]
        ids = [d["id"] for d in real]
        if len(set(ids)) != len(ids):
            fail(f"设备 id 不唯一: {ids}")
        print(f"设备: {[d['name'] for d in devices]}")

        # 空闲切设备（按稳定 id）
        ev = set_device(offerer, first["id"])
        if ev is None or ev.get("result") != "success":
            fail(f"空闲切设备失败: {ev}")
        cfg = get_config(offerer)
        dev = cfg.get("device", {})
        if dev.get("device_id") != first["id"] or \
                dev.get("device_name") != first["name"]:
            fail(f"空闲切设备后回显错误: {dev}")
        print(f"T1 空闲切到 {first['name']} ({first['id']}) OK")

        # 同名设备（两个同型号摄像头麦）按稳定 id 可分别选择
        name_counts = {}
        for d in real:
            name_counts[d["name"]] = name_counts.get(d["name"], 0) + 1
        dup = next((d for d in real if name_counts[d["name"]] > 1), None)
        if dup is not None:
            same = [d for d in real if d["name"] == dup["name"]]
            for d in same:
                ev = set_device(offerer, d["id"])
                if ev is None or ev.get("result") != "success":
                    fail(f"同名设备按 id 选择失败: {d} -> {ev}")
                cfg = get_config(offerer)
                if cfg.get("device", {}).get("device_id") != d["id"]:
                    fail(f"id 回显错误: {cfg.get('device')}")
            print(f"T2 同名设备按稳定 id 区分 OK: "
                  f"{[d['id'] for d in same]}")

        for d in (offerer, answerer):
            d.send("audio_on")
            d.wait("audio_switched", lambda e: e.get("result") == "success", 5)

        aid = None
        for _ in range(10):
            offerer.send("get_roster")
            ev = offerer.wait("signal_client.roster", timeout=5)
            if ev:
                for p in ev.get("peers", []):
                    if p["name"] == "sw_a":
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

        # 通话中切设备
        ev = set_device(offerer, second["id"])
        if ev is None or ev.get("result") != "success":
            fail(f"通话中切设备失败: {ev}")
        s = wait_audio_growing(answerer, 15)
        if s is None:
            fail("切换设备后对端收流未恢复")
        print(f"T3 通话中切到 {second['name']} OK，收流恢复 (ssrc={s['ssrc']})")

        # 非法设备：保留原设备继续收流
        offerer.send("set_audio_device no.such.device")
        ev = offerer.wait("ipc.error", timeout=5)
        if ev is None or ev.get("code") != "device_not_found":
            fail(f"非法设备错误码错误: {ev}")
        if wait_audio_growing(answerer, 10) is None:
            fail("非法设备后音频流中断")
        print("T4 非法设备 → device_not_found，音频流继续")

        print("audio device switch test ok")
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
