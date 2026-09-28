#!/usr/bin/env python3
"""Analyze a colibri .coli_usage routing history -> prune-candidate ranking.

Usage: analyze_candidates.py usage_full.txt [--pct 0.1] [--out candidates.txt]
Emits, per layer: total routed picks, distinct experts seen, the zero-route
set, and the set whose share of layer traffic is below --pct (the "cold tail"
ablation list). Output is a stable, sorted candidate list: layer expert count.
"""
import argparse, collections, json, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("usage")
    ap.add_argument("--pct", type=float, default=0.1,
                    help="candidate if layer-share below this percent")
    ap.add_argument("--out", default="candidates.txt")
    a = ap.parse_args()

    counts = collections.defaultdict(dict)     # layer -> {expert: n}
    totals = collections.defaultdict(int)
    dims = None
    for ln in open(a.usage):
        l, e, c = map(int, ln.split())
        if l < 0:
            if l == -1: dims = (e, c)          # -1 n_layers n_experts
            continue
        counts[l][e] = counts[l].get(e, 0) + c
        totals[l] += c
    if dims is None:
        sys.exit("no header record; refusing (which model is this?)")
    nl, ne = dims
    grand = sum(totals.values())
    cands = []
    print(f"layers {nl} x experts {ne} | total routed picks {grand:,}")
    for l in range(nl):
        seen = counts.get(l, {})
        zero = [e for e in range(ne) if e not in seen]
        tail = [(e, c) for e, c in seen.items()
                if totals[l] and 100.0 * c / totals[l] < a.pct]
        cands += [(l, e, 0) for e in zero] + sorted((l, e, c) for e, c in tail)
        print(f"  layer {l:>2}: picks {totals.get(l,0):>7,} | used {len(seen):>2}/{ne}"
              f" | zero-route {len(zero):>2} | <{a.pct}% tail {len(tail):>2}"
              f" | hottest {max(seen.values()) if seen else 0:>6,}")
    with open(a.out, "w") as f:
        f.write(f"# candidates from {a.usage}: layer expert count  (count=0 -> never routed)\n")
        for l, e, c in sorted(cands):
            f.write(f"{l} {e} {c}\n")
    nzero = sum(1 for _, _, c in cands if c == 0)
    print(f"\ncandidates: {nzero} zero-route + {len(cands)-nzero} below {a.pct}% = "
          f"{len(cands)}/{nl*ne} ({100*len(cands)/(nl*ne):.1f}% of container)")
    print(f"wrote {a.out}")

if __name__ == "__main__":
    main()
