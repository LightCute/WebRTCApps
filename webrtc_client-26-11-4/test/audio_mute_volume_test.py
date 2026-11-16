#!/usr/bin/env python3
"""音频软件静音 + 扬声器音量/静音测试（第二期 Task 3）。

复用 audio_config_test 的 Daemon/配置辅助函数。
断言：
  - mic_muted on：RTP 继续（包数增长），接收端 audio_level 不升高
  - mic_muted off：恢复
  - speaker_volume 0..100 回显（ADM 不支持时 available=false）
  - speaker_muted 回显
  - 以上操作均不触发重协商
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_config_test import (  # noqa: E402
    Daemon,
    SERVER,
    PORT,
    audio_streams,
    drop_event,
    fail,
    get_config,
    set_config,
    wait_audio_growing,
)


def main():
    offerer = Daemon("mute_o", "/tmp/wc_mute_o.sock")
    answerer = Daemon("mute_a", "/tmp/wc_mute_a.sock")
    try:
        offerer.start()
        answerer.start()
        offerer.connect()
        answerer.connect()
        offerer.send(f"register mute_o {SERVER} {PORT}")
        answerer.send(f"register mute_a {SERVER} {PORT}")
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
                    if p["name"] == "mute_a":
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

        cfg = get_config(offerer)
        if cfg.get("device", {}).get("mic_muted") is not False:
            fail(f"默认 mic_muted 应为 false: {cfg}")
        print("T1 默认 mic_muted=false")

        offerer.drain()
        answerer.drain()

        # 软件静音：包继续发，电平不升高
        ev = set_config(offerer, "mic_muted", "on")
        if ev is None or ev.get("device", {}).get("mic_muted") is not True:
            fail(f"mic_muted on 回显错误: {ev}")
        s = wait_audio_growing(answerer, 10)
        if s is None:
            fail("静音后 RTP 不再流动")
        time.sleep(1)
        streams = audio_streams(answerer)
        if not streams:
            fail("静音后接收端没有音频流")
        cur = max(streams, key=lambda x: x["packets_received"])
        if cur.get("audio_level", 0.0) > 0.05:
            fail(f"静音后 audio_level 仍然偏高: {cur.get('audio_level')}")
        print(f"T2 mic_muted on：RTP 继续，audio_level={cur.get('audio_level')}")

        ev = set_config(offerer, "mic_muted", "off")
        if ev is None or ev.get("device", {}).get("mic_muted") is not False:
            fail(f"mic_muted off 回显错误: {ev}")
        print("T3 mic_muted off 恢复")

        # 扬声器音量
        ev = set_config(offerer, "speaker_volume", "30")
        if ev is None:
            fail("speaker_volume 无回显")
        dev = ev.get("device", {})
        if dev.get("speaker_volume_available"):
            if dev.get("speaker_volume") != 30:
                fail(f"speaker_volume 回显应为 30，实际 {dev.get('speaker_volume')}")
            print("T4 speaker_volume=30 OK")
        else:
            print("T4 speaker_volume 不可用（ADM 无音量控制），已接受")

        # 扬声器静音
        ev = set_config(offerer, "speaker_muted", "on")
        if ev is None:
            fail("speaker_muted 无回显")
        dev = ev.get("device", {})
        if dev.get("speaker_mute_available") and not dev.get("speaker_muted"):
            fail(f"speaker_muted on 回显错误: {dev}")
        print("T5 speaker_muted on OK")

        # 无重协商
        time.sleep(1)
        for d in (offerer, answerer):
            sdp = [e for e in d.events()
                   if e.get("event") == "signal_client.local_sdp"]
            if sdp:
                fail(f"{d.role}: 静音/音量操作触发了重协商")
        print("T6 静音/音量操作无重协商")

        # 恢复
        set_config(offerer, "speaker_muted", "off")
        set_config(offerer, "speaker_volume", "100")
        set_config(offerer, "mic_muted", "off")

        print("audio mute volume test ok")
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
