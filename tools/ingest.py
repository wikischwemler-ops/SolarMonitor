#!/usr/bin/env python3
"""Nimmt Messzeilen vom ESP32 entgegen und legt sie fuer das Dashboard ab.

Eingabe:
    ROWS (oder --rows): Zeilen durch ';' getrennt, je
        ts,p,u,i,f,t,u1,i1,u2,i2,e_day,e_total[,rssi,ap,lat]
        rssi = WLAN-Signal ESP32 (dBm), ap = Signal Logger-Hotspot (dBm, 0 = nicht gefunden),
        lat = Antwortzeit des Loggers (ms). Aeltere Firmware schickt nur die ersten 12 Spalten.
    NET (oder --net), optional: "upload_ms,lost" = Dauer des vorigen Uploads, verlorene Abfragen

Ausgabe:
    docs/data/days/YYYY-MM-DD.csv   alle Messwerte eines Tages (Ortszeit Europe/Berlin)
    docs/data/summary.json          Tageswerte, Gesamtertrag, letzte Messung, Verbindung
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

BASE = ["ts", "p", "u", "i", "f", "t", "u1", "i1", "u2", "i2", "e_day", "e_total"]
NETC = ["rssi", "ap", "lat"]
COLS = BASE + NETC
TZ = ZoneInfo("Europe/Berlin")
MAX_ROWS = 2000
# Plausibilitaet: BW-MI600 kann maximal ~600 W, Gesamtzaehler < 1 GWh
LIMITS = {"p": (0, 1000), "u": (0, 300), "i": (0, 10), "f": (0, 70), "t": (-50, 120),
          "u1": (0, 80), "i1": (0, 20), "u2": (0, 80), "i2": (0, 20),
          "e_day": (0, 10), "e_total": (0, 1_000_000)}
NET_LIMITS = {"rssi": (-110, -1), "ap": (-110, -1), "lat": (1, 60_000)}


def parse_rows(raw: str) -> list[dict]:
    now = time.time()
    rows: list[dict] = []
    for part in raw.replace("\n", ";").split(";"):
        part = part.strip()
        if not part:
            continue
        fields = part.split(",")
        if len(fields) not in (len(BASE), len(COLS)):
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
        # Verbindungswerte ausserhalb des Bereichs (z. B. Hotspot nicht gefunden = 0) -> leer
        for k, (lo, hi) in NET_LIMITS.items():
            if k in row and not (lo <= row[k] <= hi):
                row[k] = None
        rows.append(row)
    return rows[:MAX_ROWS]


def parse_net(raw: str) -> dict | None:
    try:
        up_ms, lost = (int(x) for x in raw.split(","))
    except ValueError:
        return None
    if not (0 <= up_ms <= 120_000 and 0 <= lost <= 60_000):
        return None
    return {"up_ms": up_ms, "lost": lost}


ST_KEYS = ["boots", "power_ons", "reset", "wifi_ms", "fails", "rssi", "night"]


def parse_st(raw: str) -> dict | None:
    """Lebenszeichen des ESP32: Durchlaeufe, Starts ohne Tiefschlaf, Grund, WLAN-Dauer, Fehlversuche, Signal, Nacht."""
    try:
        vals = [int(x) for x in raw.split(",")]
    except ValueError:
        return None
    if len(vals) != len(ST_KEYS):
        return None
    st = dict(zip(ST_KEYS, vals))
    if not (0 <= st["wifi_ms"] <= 120_000 and -110 <= st["rssi"] <= 0 and 0 <= st["reset"] <= 20):
        return None
    st["night"] = bool(st["night"])
    return st


def fmt(v) -> str:
    if v is None:
        return ""
    return f"{v:.2f}".rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def read_day(path: Path) -> dict[int, dict]:
    out: dict[int, dict] = {}
    if path.exists():
        with path.open(newline="", encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                row = {"ts": int(r["ts"])}
                for k in COLS[1:]:
                    v = r.get(k)
                    row[k] = float(v) if v not in (None, "") else None
                out[row["ts"]] = row
    return out


def write_day(path: Path, rows: dict[int, dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(COLS)
        for ts in sorted(rows):
            w.writerow([fmt(rows[ts].get(c)) for c in COLS])


def avg(vals: list) -> float | None:
    vals = [v for v in vals if v is not None]
    return round(sum(vals) / len(vals), 1) if vals else None


def day_stats(rows: dict[int, dict], old: dict) -> dict:
    vals = [rows[t] for t in sorted(rows)]
    peak = max(vals, key=lambda r: r["p"])
    st = {
        "kwh": round(max(r["e_day"] for r in vals), 2),
        "peak_w": round(peak["p"], 1),
        "peak_ts": peak["ts"],
        "first_ts": vals[0]["ts"],
        "last_ts": vals[-1]["ts"],
        "samples": len(vals),
        "t_max": round(max(r["t"] for r in vals), 1),
    }
    for k in NETC:
        a = avg([r.get(k) for r in vals])
        if a is not None:
            st[f"{k}_avg"] = a
    # Upload-Statistik wird pro Upload hochgezaehlt, nicht aus den Zeilen berechnet
    for k in ("lost", "up_n", "up_ms_sum", "up_ms_max"):
        if k in old:
            st[k] = old[k]
    return st


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", default=os.environ.get("ROWS", ""))
    ap.add_argument("--net", default=os.environ.get("NET", ""))
    ap.add_argument("--st", default=os.environ.get("ST", ""))
    ap.add_argument("--data", default="docs/data")
    args = ap.parse_args()

    new = parse_rows(args.rows)
    esp = parse_st(args.st)
    if not new and not esp:
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
        days[d] = day_stats(merged, days.get(d, {}))
        print(f"{d}: +{len(rows)} Zeilen, {len(merged)} gesamt, {days[d]['kwh']} kWh")

    now = int(time.time())
    if new:
        latest = max(new, key=lambda r: r["ts"])
        if latest["ts"] >= summary.get("latest", {}).get("ts", 0):
            summary["latest"] = {k: latest[k] for k in COLS if latest.get(k) is not None}
            summary["total_kwh"] = latest["e_total"]
        stat_day = datetime.fromtimestamp(latest["ts"], TZ).strftime("%Y-%m-%d")
    else:
        stat_day = datetime.fromtimestamp(now, TZ).strftime("%Y-%m-%d")

    if esp:
        prev = summary.get("esp", {})
        # Neustarts ohne Tiefschlaf (Strom weg, Absturz, Spannungseinbruch) je Tag zaehlen
        if "power_ons" in prev and esp["power_ons"] > prev["power_ons"]:
            rs = summary.setdefault("restarts", {})
            rs[stat_day] = rs.get(stat_day, 0) + esp["power_ons"] - prev["power_ons"]
        summary["esp"] = {"ts": now, **esp}
        print(f"Lebenszeichen: {esp}")

    net = parse_net(args.net)
    if net and stat_day in days:
        st = days[stat_day]
        st["lost"] = st.get("lost", 0) + net["lost"]
        if net["up_ms"] > 0:                     # 0 = erster Upload nach dem Start, noch kein Wert
            st["up_n"] = st.get("up_n", 0) + 1
            st["up_ms_sum"] = st.get("up_ms_sum", 0) + net["up_ms"]
            st["up_ms_max"] = max(st.get("up_ms_max", 0), net["up_ms"])
        summary["net"] = {"ts": now, **net}

    summary["updated"] = now
    summary["days"] = dict(sorted(days.items()))
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
