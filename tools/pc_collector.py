#!/usr/bin/env python3
"""Test- und Ersatz-Collector fuer den PC (gleiches Zeilenformat wie der ESP32).

    pip install pysolarmanv5
    python tools/pc_collector.py --ip 192.168.0.155 --sn <WIFI-Nummer> --count 3 --interval 20

Gibt die Zeilen aus (fuer tools/ingest.py --rows "..."). Mit --dispatch werden sie per
repository_dispatch an GitHub geschickt (Token aus Umgebungsvariable GH_TOKEN).
"""
from __future__ import annotations

import argparse
import json
import os
import time
import urllib.request

from pysolarmanv5 import PySolarmanV5

FIRST, COUNT = 59, 54


def poll(m: PySolarmanV5) -> str:
    r = m.read_holding_registers(register_addr=FIRST, quantity=COUNT)
    R = lambda reg: r[reg - FIRST]  # noqa: E731
    vals = [
        int(time.time()),
        ((R(87) << 16) | R(86)) / 10, R(73) / 10, R(76) / 10, R(79) / 100, (R(90) - 1000) / 100,
        R(109) / 10, R(110) / 10, R(111) / 10, R(112) / 10,
        R(60) / 10, ((R(64) << 16) | R(63)) / 10,
    ]
    return ",".join([str(vals[0])] + [f"{v:.2f}" for v in vals[1:]])


def dispatch(repo: str, rows: str) -> int:
    req = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/dispatches",
        data=json.dumps({"event_type": "solar-data", "client_payload": {"v": 1, "rows": rows}}).encode(),
        headers={"Authorization": f"Bearer {os.environ['GH_TOKEN']}",
                 "Accept": "application/vnd.github+json", "User-Agent": "SolarMonitor-PC"},
        method="POST")
    with urllib.request.urlopen(req, timeout=20) as resp:
        return resp.status


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.0.155")
    ap.add_argument("--sn", type=int, required=True)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("--dispatch", metavar="OWNER/REPO")
    a = ap.parse_args()

    m = PySolarmanV5(a.ip, a.sn, port=8899, mb_slave_id=1, socket_timeout=10)
    rows = []
    for k in range(a.count):
        rows.append(poll(m))
        print(rows[-1])
        if k + 1 < a.count:
            time.sleep(a.interval)
    m.disconnect()
    joined = ";".join(rows)
    if a.dispatch:
        print("dispatch ->", dispatch(a.dispatch, joined))
    else:
        print("\nROWS=" + joined)


if __name__ == "__main__":
    main()
