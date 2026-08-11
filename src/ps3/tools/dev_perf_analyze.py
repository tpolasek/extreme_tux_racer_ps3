#!/usr/bin/env python3
"""Analyze etr_perf.log from the PS3 port.

Usage:
    src/ps3/tools/dev_perflog.sh > etr_perf.log
    src/ps3/tools/dev_perf_analyze.py [etr_perf.log]

Log format (window=1s):
    # columns: window,<tag>,<calls>,<total_us>,<avg_us>
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

LOG = Path(sys.argv[1] if len(sys.argv) > 1 else "etr_perf.log")

# Per-window rows: rows[window][tag] = (calls, total_us, avg_us)
rows = defaultdict(dict)
with LOG.open() as f:
    for row in csv.reader(f):
        if not row or row[0].startswith("#") or not row[0].isdigit():
            continue
        win, tag, calls, total_us, avg_us = (
            int(row[0]), row[1], int(row[2]), int(row[3]), int(row[4])
        )
        rows[win][tag] = (calls, total_us, avg_us)

windows = sorted(rows)
last = windows[-1]
print(f"=== etr_perf.log summary ===")
print(f"windows: {len(windows)}  ({windows[0]}..{last}, ~{len(windows)}s "
      f"= {len(windows)//60}m{len(windows)%60}s)")
print()

# Phase detection: which windows have which tag groups?
# Umbrella tags whose sum is the "race work" total.
race_work_tags = [
    "RACE_UPDATE", "RACE_RENDER_COURSE", "RACE_DRAW_TREES",
    "RACE_DRAW_PARTICLES", "RACE_DRAW_TUX", "RACE_DRAW_SNOW",
    "RACE_DRAW_HUD",
]
race_wins = [w for w in windows if "RACE_UPDATE" in rows[w]]
menu_wins = [w for w in windows if "RACE_UPDATE" not in rows[w]]
print(f"phases: race={len(race_wins)}s, menu/other={len(menu_wins)}s")
if race_wins:
    print(f"  race span: windows {race_wins[0]}..{race_wins[-1]}")
print()

# In-race phase only — aggregate per tag
print("=== In-race per-tag aggregates (race windows only) ===")
print(f"{'tag':<22}{'calls':>9}{'tot_us':>13}{'avg_us/call':>13}"
      f"{'us/frame':>11}{'% frame':>9}")
# Compute total_us summed across windows and total calls
agg = defaultdict(lambda: [0, 0])  # tag -> [calls, total_us]
for w in race_wins:
    for tag, (c, t, a) in rows[w].items():
        agg[tag][0] += c
        agg[tag][1] += t

# Per-frame breakdown: us/frame = total_us / total_main_loop_calls
ml_calls = agg["MAIN_LOOP"][0]
ml_total = agg["MAIN_LOOP"][1]
frame_budget_us = 1_000_000 / 60  # 16666
print(f"(race frames captured: {ml_calls}, "
      f"avg MAIN_LOOP = {ml_total/ml_calls:.0f} us "
      f"= {1e6/(ml_total/ml_calls):.1f} FPS)\n")

ordered = sorted(agg.items(), key=lambda kv: -kv[1][1])
for tag, (c, t) in ordered:
    avg = t / c if c else 0
    per_frame = t / ml_calls if ml_calls else 0
    pct = 100 * per_frame / frame_budget_us
    print(f"{tag:<22}{c:>9}{t:>13}{avg:>13.1f}{per_frame:>11.0f}{pct:>8.1f}%")

print()
print("=== Race frame-time composition (us/frame, race windows only) ===")
# Re-derive per-frame for race-only tags
print(f"{'tag':<22}{'us/frame':>10}{'stdev':>9}{'min':>7}{'max':>7}"
      f"{'% frame':>9}")
# Build per-window us/frame series for each tag
series = defaultdict(list)
for w in race_wins:
    fr = rows[w]["MAIN_LOOP"][0]  # frames in this window
    for tag in (race_work_tags +
                ["MAIN_LOOP", "GL_FLUSH", "GL_FLUSH_WAIT",
                 "FLIP", "CLEAR", "FLUSH_FP_PATCHES", "FREE_TIME"]):
        if tag in rows[w]:
            c, t, _ = rows[w][tag]
            series[tag].append(t / fr if fr else 0)

for tag in [t for t, _ in ordered]:
    if tag not in series:
        continue
    s = series[tag]
    if not s:
        continue
    mean = statistics.mean(s)
    pct = 100 * mean / frame_budget_us
    sd = statistics.pstdev(s) if len(s) > 1 else 0
    print(f"{tag:<22}{mean:>10.0f}{sd:>9.0f}{min(s):>7.0f}{max(s):>7.0f}"
          f"{pct:>8.1f}%")

print()
print("=== Notes ===")
race_work = sum(statistics.mean(series[t]) for t in race_work_tags
                if t in series)
free = statistics.mean(series["FREE_TIME"]) if series["FREE_TIME"] else 0
ml = statistics.mean(series["MAIN_LOOP"]) if series["MAIN_LOOP"] else 0
print(f"avg race work (sum of RACE_*):   {race_work:7.0f} us/frame "
      f"({100*race_work/frame_budget_us:.1f}% of budget)")
print(f"avg FREE_TIME reported:          {free:7.0f} us/frame "
      f"({100*free/frame_budget_us:.1f}% of budget)")
print(f"avg MAIN_LOOP total:             {ml:7.0f} us/frame "
      f"({100*ml/frame_budget_us:.1f}% of budget -> "
      f"{1e6/ml:.1f} FPS)")
print(f"budget @60Hz:                    {frame_budget_us:.0f} us/frame")
# What's the biggest single RACE cost?
biggest = max(race_work_tags, key=lambda t: statistics.mean(series[t]))
print(f"biggest in-race cost: {biggest} "
      f"({statistics.mean(series[biggest]):.0f} us/frame)")
