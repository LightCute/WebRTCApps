#!/usr/bin/env python3
"""SIGKILL 专项抗压测试：offer_sigkill + answer_sigkill，2 方向 × 5 遍断连。"""

import sys
import time

from reconnect_integration_test import DIRECTIONS, ITEMS, run_item


def main():
    items = [it for it in ITEMS if it[0].endswith("_sigkill")]
    results = []
    t0 = time.time()
    for d in DIRECTIONS:
        for item in items:
            r = run_item(d, item, 5)
            results.append(r)
            status = "PASS" if not r["fail"] else "FAIL"
            print(f"[{status}] {d} {r['item']}: connected={r['connected']} "
                  f"teardowns={r['teardowns']}"
                  + (f" fail={r['fail']}" if r["fail"] else ""), flush=True)
    failed = [r for r in results if r["fail"]]
    print(f"\n=== sigkill 汇总：{len(results)} 项，通过 {len(results) - len(failed)}，"
          f"失败 {len(failed)}，用时 {time.time() - t0:.0f}s ===")
    for r in failed:
        print(f"  FAIL {r['direction']} {r['item']}: {r['fail']}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
