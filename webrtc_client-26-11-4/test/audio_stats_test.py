#!/usr/bin/env python3
"""音频统计 + 远端音频事件测试（第二期 Task 7）。

断言：
  - audio_on + call 后双方收到 remote_audio.started
  - set_audio_stats on 1s：audio_stats 周期上报，包数递增、codec=opus、
    APM 字段存在
  - 通话中 switch_ice 重建 PC 后统计链不断
  - audio_off 后对端收到 remote_audio.stopped
  - set_audio_stats off 后停止推送
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_config_test import (  # noqa: E402
    Daemon,
    SERVER,
    PORT,
    drop_event,
    fail,
    wait_audio_growing,
)


def main():
    offerer = Daemon("st_o", "/tmp/wc_st_o.sock")
    answerer = Daemon("st_a", "/tmp/wc_st_a.sock")
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        offerer.send(f"register st_o {SERVER} {PORT}")
        answerer.send(f"register st_a {SERVER} {PORT}")
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
                    if p["name"] == "st_a":
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

        # 远端音频事件
        for d in (offerer, answerer):
            ev = d.wait("remote_audio.started", timeout=10)
            if ev is None:
                fail(f"{d.role}: 未收到 remote_audio.started")
        print("T1 双方 remote_audio.started")

        # 开启统计
        offerer.send("set_audio_stats on 1000")
        st1 = offerer.wait("audio_stats", timeout=8)
        if st1 is None:
            fail("未收到 audio_stats")
        if not st1.get("recv", {}).get("codec", "").endswith("opus"):
            fail(f"recv codec 不是 opus: {st1.get('recv')}")
        if st1["recv"].get("packets_received", 0) <= 0:
            fail(f"recv 包数为 0: {st1.get('recv')}")
        if st1["send"].get("packets_sent", 0) <= 0:
            fail(f"send 包数为 0: {st1.get('send')}")
        if "apm" not in st1 or "residual_echo_likelihood" not in st1["apm"]:
            fail(f"apm 字段缺失: {st1}")
        st2 = offerer.wait("audio_stats", timeout=8)
        if st2 is None or \
                st2["recv"]["packets_received"] <= st1["recv"]["packets_received"]:
            fail(f"统计包数未递增: {st1.get('recv')} -> {st2 and st2.get('recv')}")
        print(f"T2 audio_stats 递增: recv {st1['recv']['packets_received']}"
              f"→{st2['recv']['packets_received']}, send "
              f"{st1['send']['packets_sent']}→{st2['send']['packets_sent']}")

        # PC 重建后统计链不断
        offerer.send("switch_ice relay")
        if not offerer.wait("ice_switched",
                            lambda e: e.get("result") == "success", 45):
            fail("switch_ice relay 未完成")
        time.sleep(1)
        drop_event(offerer, "audio_stats")
        st3 = offerer.wait("audio_stats", timeout=10)
        if st3 is None:
            fail("PC 重建后 audio_stats 链断了")
        st4 = offerer.wait("audio_stats", timeout=8)
        if st4 is None or \
                st4["recv"]["packets_received"] <= st3["recv"]["packets_received"]:
            fail(f"重建后统计未继续递增: {st3.get('recv')} -> {st4 and st4.get('recv')}")
        print("T3 switch_ice 重建后统计链持续")

        # audio_off → 对端 remote_audio.stopped
        offerer.send("audio_off")
        if not offerer.wait("audio_switched",
                            lambda e: e.get("result") == "success"
                            and e.get("enabled") is False, 20):
            fail("audio_off 未完成")
        ev = answerer.wait("remote_audio.stopped", timeout=15)
        if ev is None:
            fail("对端未收到 remote_audio.stopped")
        print("T4 audio_off → 对端 remote_audio.stopped")

        # 关闭统计
        offerer.send("set_audio_stats off")
        time.sleep(1)
        drop_event(offerer, "audio_stats")
        if offerer.wait("audio_stats", timeout=2) is not None:
            fail("set_audio_stats off 后仍在推送")
        print("T5 set_audio_stats off 停止推送")

        # 非法参数
        offerer.send("set_audio_stats bogus")
        ev = offerer.wait("ipc.error", timeout=5)
        if ev is None or ev.get("code") != "usage":
            fail(f"set_audio_stats 非法参数错误: {ev}")
        print("T6 非法参数 → usage")

        print("audio stats test ok")
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
