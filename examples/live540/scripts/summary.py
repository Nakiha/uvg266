#!/usr/bin/env python3
"""Summarize completed frames in AU return order. Times are milliseconds."""
import argparse
import csv
import json
import math
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    return round(ordered[math.ceil(fraction * len(ordered)) - 1], 3)


def summarize(path):
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError("empty trace")
    stamps = {name: [int(r[name + "_ns"]) for r in rows]
              for name in ("due", "arrival", "submit", "ready")}
    pts = [int(r["pts"]) for r in rows]
    if pts != list(range(len(rows))):
        raise ValueError("missing, reordered or duplicate low-delay PTS")
    deadlines, arrivals, submits, ready = stamps.values()
    for i in range(len(rows)):
        if not deadlines[i] <= arrivals[i] <= submits[i] <= ready[i]:
            raise ValueError(f"invalid timestamps at PTS {pts[i]}")
        if i and ready[i] < ready[i - 1]:
            raise ValueError("nonmonotonic AU return time")
    latency = [(r - a) / 1e6 for r, a in zip(ready, arrivals)]
    gap = [(r - prev) / 1e6 for prev, r in zip(ready, ready[1:])]
    delay = [(a - d) / 1e6 for a, d in zip(arrivals, deadlines)]
    key = [i for i, row in enumerate(rows) if int(row["nal_type"]) in (7, 8, 9)]
    worst = gap.index(max(gap)) + 1 if gap else None
    total_bytes = sum(int(r["bytes"]) for r in rows)
    return {
        "frames": len(rows), "au_bytes": total_bytes,
        "media_kbps": round(total_bytes * 8 * 30 / (1000 * len(rows)), 2),
        "input_mean_fps": round((len(rows) - 1) * 1e9 / (arrivals[-1] - arrivals[0]), 4) if gap else None,
        "arrival_late_p95_ms": percentile(delay, .95), "arrival_late_max_ms": round(max(delay), 3),
        "arrival_to_ready_p50_ms": percentile(latency, .5),
        "arrival_to_ready_p95_ms": percentile(latency, .95),
        "arrival_to_ready_p99_ms": percentile(latency, .99),
        "arrival_to_ready_max_ms": round(max(latency), 3),
        "queue_wait_p95_ms": percentile([(s - a) / 1e6 for s, a in zip(submits, arrivals)], .95),
        "submit_to_ready_p95_ms": percentile([(r - s) / 1e6 for r, s in zip(ready, submits)], .95),
        "out_gap_p95_ms": percentile(gap, .95) if gap else None,
        "out_gap_max_ms": round(max(gap), 3) if gap else None,
        "out_gap_max_pts_pair": [pts[worst - 1], pts[worst]] if gap else None,
        "first_au_ms": round(latency[0], 3),
        "key_pts": key,
        "recurring_key_max_latency_ms": round(max(latency[i] for i in key if i), 3) if any(key) else None,
        "max_queued_ahead": max(int(r["queue_ahead"]) for r in rows),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    print(json.dumps(summarize(parser.parse_args().trace), indent=2))
