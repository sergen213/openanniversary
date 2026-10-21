#!/usr/bin/env python3
"""Summarise a d3d9hook capture (tools/dxhook) to answer GEOMETRY.md 17: does a terrain strip draw add a
per-material base vertex index, or are the strip indices absolute?

Streaming on purpose - a two-minute capture is over a gigabyte of text - so memory stays bounded by the
number of distinct buffer pairs, not the number of draws.

usage: report.py <capture dir>     # wine-tra/drive_c/tra/d3d9log
"""
import os
import re
import sys
from collections import defaultdict

LINE = re.compile(r"^[0-9a-f]{8} (\w+) (.*)$")
DRAW = re.compile(r"type=(\d+) bvi=(-?\d+) minv=(\d+) numv=(\d+) starti=(\d+) prims=(\d+) stride=(\d+) off=(\d+) vb=(\w+) ib=(\w+)")

MAX_BVI = 8192  # ponytail: distinct-base sets are capped; a level uses far fewer per buffer


def main(path):
    stats = defaultdict(lambda: {"draws": 0, "tris": 0, "bvi": set(), "bvi_capped": False,
                                 "reach": 0, "strides": set(), "offs": set()})
    kinds = defaultdict(int)
    total_draws = big_draws = 0
    with open(os.path.join(path, "d3d9.txt"), errors="replace") as f:
        for raw in f:
            m = LINE.match(raw)
            if not m:
                continue
            kind, rest = m.groups()
            kinds[kind] += 1
            if kind != "draw":
                continue
            d = DRAW.match(rest)
            if not d:
                continue
            typ, bvi, minv, numv, starti, prims, stride, off = (int(x) for x in d.groups()[:8])
            vb, ib = (int(x, 16) for x in d.groups()[8:])
            if prims < 16:  # UI quads and small props cannot carry a terrain base
                continue
            big_draws += 1
            s = stats[(vb, ib)]
            s["draws"] += 1
            s["tris"] += prims
            if bvi not in s["bvi"]:
                if len(s["bvi"]) < MAX_BVI:
                    s["bvi"].add(bvi)
                else:
                    s["bvi_capped"] = True
            reach = bvi + numv
            if reach > s["reach"]:
                s["reach"] = reach
            s["strides"].add(stride)
            s["offs"].add(off)

    print("line kinds:", dict(kinds))
    print(f"draw lines {total_draws}, with prims>=16: {big_draws}")
    for pref in ("vb", "ib"):
        files = [n for n in os.listdir(path) if n.startswith(pref + "_")]
        tot = sum(os.path.getsize(os.path.join(path, n)) for n in files)
        print(f"{pref} dump files: {len(files)} ({tot/1e6:.2f} MiB)")

    nz = sum(1 for s in stats.values() if s["bvi"] != {0})
    print(f"\nbuffer pairs used by terrain-shaped draws: {len(stats)}; "
          f"those whose base vertex index is ever non-zero: {nz}")
    top = sorted(stats.items(), key=lambda kv: -kv[1]["tris"])[:12]
    for (vb, ib), s in top:
        bases = sorted(s["bvi"])
        print(f"  vb=0x{vb:08x} ib=0x{ib:08x} draws={s['draws']:6d} tris={s['tris']:7d} "
              f"max_reach={s['reach']:8d} strides={sorted(s['strides'])[:4]} offs={sorted(s['offs'])[:4]}")
        print(f"      distinct bvi: {bases[:10]}{' ...' if s['bvi_capped'] else ''}")

    # the vertex block each pair reads must be big enough for max_reach vertices; infer its stride from
    # a dump only when one exists, otherwise report what the draws imply about size
    print("\nif indices are absolute (all bvi=0), a draw's reach is a direct index into the unit's "
          "vertex block; non-zero bases would mean the engine adds a per-material offset at draw time.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
