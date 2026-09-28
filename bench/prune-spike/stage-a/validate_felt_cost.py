#!/usr/bin/env python3
"""Validate the simulator's felt-cost model against the real engine.

`residency_sim.py` assumes felt wait is proportional to MISS COUNT: each miss
costs `felt_miss_us`, hits cost nothing, and the policy that reduces misses most
wins. That assumption is what Stage A's whole verdict rests on, so it is worth
testing rather than inheriting.

Two things it cannot express, both measurable here:
  1. A read can be OVERLAPPED with compute. If prefetch hides it, a miss is not
     felt wait at all — the engine's PROF line reports both "read service" and
     "felt wait" separately, and they differ.
  2. More residency can be SLOWER. The earlier cache sweep on this box showed
     cap=48 at 92.0% hit running slower than cap=32 at 80.2% hit.

So: run the same ref at several --cap (which sets both misses and bytes resident),
record wall-clock and misses, and fit seconds-per-miss. If the fit is bad, the
simulator's felt-wait ranking is a weaker predictor of tok/s than it looks, and
the Stage-A verdict has to say so.

Usage: validate_felt_cost.py <binary> <ref> <cap...>
"""
import re
import statistics
import subprocess
import sys
from pathlib import Path

MODEL = str(Path.home() / "models" / "olmoe_merged")


def run(binary, ref, cap, threads=6):
    env = {"SNAP": MODEL, "PPL": "1", "OMP_NUM_THREADS": str(threads),
           "PATH": "/usr/bin:/bin"}
    out = subprocess.run([binary, str(cap), "8", ref], env=env,
                         capture_output=True, text=True).stdout
    m = re.search(r"hit rate: ([\d.]+)%\s+\(hit=(\d+) miss=(\d+)\)", out)
    t = re.search(r"Speed: ([\d.]+) tok/s \(([\d.]+)s for (\d+) tokens\)", out)
    nll = re.search(r"TF-NLL: ([\d.]+)", out)
    if not (m and t):
        raise RuntimeError(f"cap={cap}: unparsable output:\n{out[-500:]}")
    return {"cap": cap, "hit_pct": float(m.group(1)), "hits": int(m.group(2)),
            "misses": int(m.group(3)), "tok_s": float(t.group(1)),
            "seconds": float(t.group(2)), "tokens": int(t.group(3)),
            "nll": float(nll.group(1))}


def main():
    binary, ref = sys.argv[1], sys.argv[2]
    caps = [int(c) for c in sys.argv[3:]] or [8, 16, 24, 32, 48, 64]
    rows = [run(binary, ref, c) for c in caps]

    print(f"{'cap':>4} {'hit%':>7} {'misses':>8} {'seconds':>9} {'tok/s':>7} "
          f"{'us/miss':>9} {'nll':>7}")
    for r in rows:
        us = r["seconds"] * 1e6 / r["misses"]
        print(f"{r['cap']:4d} {r['hit_pct']:7.2f} {r['misses']:8d} "
              f"{r['seconds']:9.2f} {r['tok_s']:7.2f} {us:9.1f} {r['nll']:7.4f}")

    # Is seconds a linear function of misses? Fit and report the residual spread;
    # a wide band means "fewer misses" does not reliably mean "faster", which is
    # the assumption the Phase-0 gate's felt-wait metric makes.
    xs = [r["misses"] for r in rows]
    ys = [r["seconds"] for r in rows]
    n = len(xs)
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    denom = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / denom if denom else 0.0
    intercept = my - slope * mx
    resid = [y - (intercept + slope * x) for x, y in zip(xs, ys)]
    rms = (sum(r * r for r in resid) / n) ** 0.5
    print(f"\nlinear fit: seconds = {intercept:.2f} + {slope*1e6:.1f}us * misses"
          f"   (RMS residual {rms:.2f}s over a {max(ys)-min(ys):.2f}s range)")
    tied = [r for r in rows]
    if rms > 0.15 * (max(ys) - min(ys)):
        print("WARNING: residuals are large relative to the range — miss count alone")
        print("         does not explain runtime here, so a felt-wait ranking cannot")
        print("         be read as a tok/s ranking.")
    nlls = {round(r["nll"], 4) for r in rows}
    print(f"PPL across caps: {sorted(nlls)} {'(token-exact: placement only)' if len(nlls)==1 else '(CHANGED — investigate)'}")


if __name__ == "__main__":
    main()
