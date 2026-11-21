#!/usr/bin/env python3
"""Nimmt Messzeilen vom ESP32 entgegen und legt sie fuer das Dashboard ab.

Eingabe (Umgebungsvariable ROWS oder --rows): Zeilen durch ';' getrennt, je
    ts,p,u,i,f,t,u1,i1,u2,i2,e_day,e_total

Ausgabe:
    docs/data/days/YYYY-MM-DD.csv   alle Messwerte eines Tages (Ortszeit Europe/Berlin)
    docs/data/summary.json          Tageswerte, Gesamtertrag, letzte Messung
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
import time
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

COLS = ["ts", "p", "u", "i", "f", "t", "u1", "i1", "u2", "i2", "e_day", "e_total"]
TZ = ZoneInfo("Europe/Berlin")
MAX_ROWS = 2000
# Plausibilitaet: BW-MI600 kann maximal ~600 W, Gesamtzaehler < 1 GWh
LIMITS = {"p": (0, 1000), "u": (0, 300), "i": (0, 10), "f": (0, 70), "t": (-50, 120),
          "u1": (0, 80), "i1": (0, 20), "u2": (0, 80), "i2": (0, 20),
          "e_day": (0, 10), "e_total": (0, 1_000_000)}


def parse_rows(raw: str) -> list[dict]:
    now = time.time()
    rows: list[dict] = []
    for part in raw.replace("\n", ";").split(";"):
        part = part.strip()
        if not part:
            continue
        fields = part.split(",")
        if len(fields) != len(COLS):
            print(f"uebersprungen (Spalten): {part[:80]!r}", file=sys.stderr)
            continue
        try:
            ts = int(fields[0])
            vals = [float(x) for x in fields[1:]]
        except ValueError:
            print(f"uebersprungen (Zahl): {part[:80]!r}", file=sys.stderr)
            continue
        if not (1_700_000_000 < ts < now + 600) or any(not math.isfinite(v) for v in vals):
            print(f"uebersprungen (Zeit/Wert): {part[:80]!r}", file=sys.stderr)
            continue
        row = {"ts": ts, **dict(zip(COLS[1:], vals))}
        if any(not (lo <= row[k] <= hi) for k, (lo, hi) in LIMITS.items()):
            print(f"uebersprungen (Grenzwert): {part[:80]!r}", file=sys.stderr)
            continue
        rows.append(row)
    return rows[:MAX_ROWS]


def fmt(v: float) -> str:
    return f"{v:.2f}".rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def read_day(path: Path) -> dict[int, dict]:
    out: dict[int, dict] = {}
    if path.exists():
        with path.open(newline="", encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                out[int(r["ts"])] = {"ts": int(r["ts"]), **{k: float(r[k]) for k in COLS[1:]}}
    return out


def write_day(path: Path, rows: dict[int, dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(COLS)
        for ts in sorted(rows):
            w.writerow([fmt(rows[ts][c]) for c in COLS])


def day_stats(rows: dict[int, dict]) -> dict:
    vals = [rows[t] for t in sorted(rows)]
    peak = max(vals, key=lambda r: r["p"])
    return {
        "kwh": round(max(r["e_day"] for r in vals), 2),
        "peak_w": round(peak["p"], 1),
        "peak_ts": peak["ts"],
        "first_ts": vals[0]["ts"],
        "last_ts": vals[-1]["ts"],
        "samples": len(vals),
        "t_max": round(max(r["t"] for r in vals), 1),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", default=os.environ.get("ROWS", ""))
    ap.add_argument("--data", default="docs/data")
    args = ap.parse_args()

    new = parse_rows(args.rows)
    if not new:
        print("keine gueltigen Zeilen")
        return 0

    data = Path(args.data)
    by_day: dict[str, list[dict]] = {}
    for r in new:
        d = datetime.fromtimestamp(r["ts"], TZ).strftime("%Y-%m-%d")
        by_day.setdefault(d, []).append(r)

    summary_path = data / "summary.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.exists() else {}
    days = summary.setdefault("days", {})

    for d, rows in by_day.items():
        path = data / "days" / f"{d}.csv"
        merged = read_day(path)
        for r in rows:
            merged[r["ts"]] = r
        write_day(path, merged)
        days[d] = day_stats(merged)
        print(f"{d}: +{len(rows)} Zeilen, {len(merged)} gesamt, {days[d]['kwh']} kWh")

    latest = max(new, key=lambda r: r["ts"])
    if latest["ts"] >= summary.get("latest", {}).get("ts", 0):
        summary["latest"] = {k: latest[k] for k in COLS}
        summary["total_kwh"] = latest["e_total"]
    summary["updated"] = int(time.time())
    summary["days"] = dict(sorted(days.items()))
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
