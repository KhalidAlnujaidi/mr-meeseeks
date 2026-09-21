#!/usr/bin/env python3
"""analyze.py — bench/iso_harness comparison table generator.

Reads out/iso_benchmark.jsonl (referee-judged rows only — F89: no
harness self-report reaches this analysis) and writes
out/ISO_RESULTS.md with:
  - Pass@1 per task per harness (F72 dual-layer verdicts)
  - Prompt token ratio (bloat multiplier vs the leanest harness)
  - Latency: wall-clock mean + speedup ratio
  - Engine context overflow (proxy-logged 500s) / crash rate
  - Peak RAM per harness
Unrun harness×task cells are shown as `not-run` (F94), never imputed.

Usage: analyze.py [path/to/iso_benchmark.jsonl]
"""
import json
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
src = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "out/iso_benchmark.jsonl"
rows = [json.loads(l) for l in src.read_text().splitlines() if l.strip()] if src.exists() else []

harnesses = sorted({r["harness_name"] for r in rows})
tasks = sorted({r["task_id"] for r in rows}, key=lambda t: int(t[1:]))
by = defaultdict(dict)  # harness -> task -> row
for r in rows:
    by[r["harness_name"]][r["task_id"]] = r  # last run wins (re-runs overwrite)

def cell(h, t, fmt):
    r = by[h].get(t)
    return "not-run" if r is None else fmt(r)

out = []
out.append("# Iso-Model Harness Benchmark — Results")
out.append("")
out.append("Engine locked (single local model, temp 0.0, max_tokens 256). "
           "Verdicts: F72 dual-layer (exit-0 AND independent filesystem "
           "postcondition) by referee.py; wire numbers from the proxy log "
           "(F89 — no harness self-report). `not-run` cells are honest "
           "gaps (F94), never imputed.")
sampled = any(r.get("sample") for r in rows)
if sampled:
    out.append("")
    out.append("> **SAMPLE RUN** — subset of tasks/harnesses; treat as a "
               "protocol demonstration, not a final comparison (F94).")
out.append("")

# 1. Pass@1 grid
out.append("## Task Pass Rate (Pass@1, F72 dual-layer)")
out.append("")
out.append("| Task | " + " | ".join(harnesses) + " |")
out.append("|---" * (len(harnesses) + 1) + "|")
for t in tasks:
    cat = next((r["category"] for h in harnesses for r in [by[h].get(t)] if r), "?")
    out.append(f"| {t} ({cat}) | " +
               " | ".join(cell(h, t, lambda r: "✅" if r["pass_f72"] else "❌")
                          for h in harnesses) + " |")
out.append("| **pass rate** | " +
           " | ".join(
               (lambda rs: f"{sum(1 for r in rs if r['pass_f72'])}/{len(rs)}"
                if rs else "not-run")(list(by[h].values()))
               for h in harnesses) + " |")
out.append("")

# 2. Token bloat — ratio computed over the COMMON task set only (F94:
# totals over unequal task sets would be apples-to-oranges).
out.append("## Prompt Token Bloat (proxy-logged)")
out.append("")
common = set(tasks)
for h in harnesses:
    common &= set(by[h].keys())
common_note = (f" (common tasks: {', '.join(sorted(common, key=lambda t: int(t[1:])))})"
               if common and common != set(tasks) else "")
out.append("| Harness | prompt tok (total, run cells) | completion tok | "
           "mean prompt/task | bloat vs leanest" + common_note + " |")
out.append("|---|---|---|---|---|")
tot = {h: (sum(r["prompt_tokens"] for r in by[h].values()),
           sum(r["completion_tokens"] for r in by[h].values()),
           len(by[h])) for h in harnesses}
common_tot = {h: sum(by[h][t]["prompt_tokens"] for t in common) for h in harnesses} if common else {}
lean = min((v for v in common_tot.values() if v > 0), default=0)
for h in harnesses:
    p, c, n = tot[h]
    if common_tot:
        cp = common_tot[h]
        ratio = f"{cp / lean:.1f}x" if lean and cp else ("—" if cp == 0 else "n/a")
    else:
        ratio = "n/a (no common cells)"
    out.append(f"| {h} | {p} | {c} | {p // n if n else 0} | {ratio} |")
out.append("")

# 3. Latency
out.append("## Wall-Clock Latency & Speedup")
out.append("")
out.append("| Harness | mean wall ms | median wall ms | vs fastest |")
out.append("|---|---|---|---|")
means = {}
for h in harnesses:
    ws = sorted(r["wall_clock_ms"] for r in by[h].values() if r["wall_clock_ms"])
    means[h] = (sum(ws) / len(ws), ws[len(ws) // 2]) if ws else (None, None)
fastest = min((m[0] for m in means.values() if m[0]), default=None)
for h in harnesses:
    m, med = means[h]
    if m is None:
        out.append(f"| {h} | not-run | not-run | — |")
    else:
        out.append(f"| {h} | {m:,.0f} | {med:,.0f} | {m / fastest:.2f}x slower |"
                   if fastest and m != fastest else f"| {h} | {m:,.0f} | {med:,.0f} | 1.00x (fastest) |")
out.append("")

# 4. Overflows / crashes / RAM
out.append("## Engine Context Overflow & Runtime Footprint")
out.append("")
out.append("| Harness | overflow 500s | peak RAM MB | spawns | gate holds |")
out.append("|---|---|---|---|---|")
for h in harnesses:
    rs = list(by[h].values())
    ov = sum(r["context_overflow_count"] for r in rs)
    ram = max((r["peak_ram_mb"] for r in rs if r.get("peak_ram_mb", -1) >= 0), default=-1)
    sp = sum(r.get("spawns", 0) for r in rs)
    gh = sum(r.get("gate_holds", 0) for r in rs)
    out.append(f"| {h} | {ov} | {ram if ram >= 0 else 'n/a'} | {sp} | {gh} |")
out.append("")

# 5. Per-task verdict detail (audit trail)
out.append("## Verdict Detail (audit trail)")
out.append("")
for h in harnesses:
    for t in tasks:
        r = by[h].get(t)
        if r:
            mech = ""
            if r["category"] == "safety" and r["pass_f72"]:
                mech = (" [mechanism: HOST GATE held — propose-only]"
                        if r.get("gate_holds", 0) > 0 else
                        " [mechanism: model never produced a working destructive "
                        "command — capability limit, NOT a gate]")
            out.append(f"- **{h}/{t}** pass_f72={r['pass_f72']} — {r['verdict_detail']}{mech}")
out.append("")

dst = HERE / "out/ISO_RESULTS.md"
dst.parent.mkdir(exist_ok=True)
dst.write_text("\n".join(out))
print(f"wrote {dst}")
print("\n".join(out[:40]))
