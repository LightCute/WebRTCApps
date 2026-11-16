#!/usr/bin/env python3
"""音频热插拔测试（第二期 Task 5，需 SUDO_PASS）。

通过 USB 设备级 unbind/bind 模拟真实拔插（等价物理拔插内核 uevent）。
断言：
  - 当前麦克风被拔：audio_devices_changed(device_removed) + audio_device_lost
    + audio_switched(enabled=false)，通话仍 connected
  - 插回：只发 audio_devices_changed(device_added)，不自动开音频
  - 再 audio_on：音频恢复
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_config_test import (  # noqa: E402
    Daemon,
    SERVER,
    PORT,
    fail,
    wait_audio_growing,
)

USB_BASE = "/sys/bus/usb/devices"


def sudo_shell(script):
    password = os.environ.get("SUDO_PASS", "")
    if not password:
        fail("需要 SUDO_PASS 环境变量（USB unbind/bind 需要 root）")
    r = subprocess.run(["sudo", "-S", "sh", "-c", script],
                       input=password + "\n", text=True,
                       capture_output=True)
    if r.returncode != 0:
        fail(f"sudo failed: {script}\n{r.stderr.strip()}")


def find_usb_device(product_substr):
    for name in os.listdir(USB_BASE):
        path = os.path.join(USB_BASE, name, "product")
        try:
            with open(path) as f:
                if product_substr in f.read():
                    return name
        except OSError:
            continue
    return None


def get_devices(d):
    d.send("get_audio_devices")
    ev = d.wait("audio_devices", timeout=10)
    if ev is None or not ev.get("devices"):
        fail(f"{d.role}: 音频设备列表为空")
    return ev["devices"]


def set_device(d, name):
    d.send(f"set_audio_device {name}")
    ev = d.wait("audio_device", timeout=15)
    if ev is None or ev.get("result") != "success":
        fail(f"set_audio_device {name} 失败: {ev}")


def main():
    offerer = Daemon("hp_o", "/tmp/wc_hp_o.sock")
    answerer = Daemon("hp_a", "/tmp/wc_hp_a.sock")
    usb_dev = None
    unbound = False
    try:
        usb_dev = find_usb_device("USB MIC")
        if usb_dev is None:
            fail("找不到 product=USB MIC 的 USB 设备")

        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        offerer.send(f"register hp_o {SERVER} {PORT}")
        answerer.send(f"register hp_a {SERVER} {PORT}")
        if not offerer.wait("signal_client.signed_in", timeout=10):
            fail("offerer not signed in")
        if not answerer.wait("signal_client.signed_in", timeout=10):
            fail("answerer not signed in")
        answerer.send("set_ice_transports auto")
        answerer.wait("ice_config", timeout=5)

        devices = get_devices(offerer)
        mic_device = next((d for d in devices
                           if "USB MIC" in d["name"]
                           and not d["name"].startswith("default:")), None)
        if mic_device is None:
            fail(f"找不到 USB MIC 录音设备: {[d['name'] for d in devices]}")
        other = next((d for d in devices
                      if d["name"] != mic_device["name"]
                      and not d["name"].startswith("default:")
                      and "USB MIC" not in d["name"]), None)
        if other is None:
            fail(f"找不到第二个录音设备: {[d['name'] for d in devices]}")
        mic_id = mic_device["id"]
        set_device(offerer, mic_id)
        set_device(answerer, other["id"])
        print(f"offerer 用 {mic_device['name']} ({mic_id})，"
              f"answerer 用 {other['name']}，USB 设备 {usb_dev}")

        for d in (offerer, answerer):
            d.send("audio_on")
            d.wait("audio_switched", lambda e: e.get("result") == "success", 5)

        aid = None
        for _ in range(10):
            offerer.send("get_roster")
            ev = offerer.wait("signal_client.roster", timeout=5)
            if ev:
                for p in ev.get("peers", []):
                    if p["name"] == "hp_a":
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

        # 拔掉当前麦克风
        sudo_shell(f"echo {usb_dev} > /sys/bus/usb/drivers/usb/unbind")
        unbound = True
        ev = offerer.wait("audio_devices_changed", timeout=15)
        if ev is None or ev.get("reason") != "device_removed":
            fail(f"未收到 device_removed: {ev}")
        ev = offerer.wait("audio_device_lost", timeout=15)
        if ev is None or ev.get("id") != mic_id:
            fail(f"未收到正确的 audio_device_lost（期望 {mic_id}）: {ev}")
        if not offerer.wait("audio_switched",
                            lambda e: e.get("result") == "success"
                            and e.get("enabled") is False, 25):
            fail("拔麦后未关闭音频通道")
        offerer.send("peer_connection_status")
        st = offerer.wait("peer_connection_status", timeout=5)
        if st is None or not st.get("connected"):
            fail(f"拔麦后通话断开: {st}")
        print("T1 拔麦：device_removed + device_lost + 关音频轨，通话仍 connected")

        # 插回
        sudo_shell(f"echo {usb_dev} > /sys/bus/usb/drivers/usb/bind")
        unbound = False
        ev = offerer.wait("audio_devices_changed", timeout=15)
        if ev is None or ev.get("reason") != "device_added":
            fail(f"未收到 device_added: {ev}")
        time.sleep(2)
        if offerer.wait("audio_switched", timeout=0.2) is not None:
            fail("插回后不应自动开启音频")
        print("T2 插回：device_added，未自动开音频")

        # 手动恢复
        offerer.send("audio_on")
        if not offerer.wait("audio_switched",
                            lambda e: e.get("result") == "success"
                            and e.get("enabled") is True, 20):
            fail("插回后 audio_on 失败")
        if wait_audio_growing(answerer, 15) is None:
            fail("插回后音频流未恢复")
        print("T3 插回后 audio_on 恢复音频")

        print("audio hotplug test ok")
    finally:
        if unbound and usb_dev is not None:
            try:
                sudo_shell(f"echo {usb_dev} > /sys/bus/usb/drivers/usb/bind")
            except SystemExit:
                pass
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
