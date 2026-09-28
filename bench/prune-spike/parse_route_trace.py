#!/usr/bin/env python3
"""Convert a colibri ROUTE_TRACE capture (the #700 format:
`<call> <row> <layer> <id>:<gate.4f> ...` per MoE call) into the
.coli_usage-compatible counts file the prune pipeline consumes, AND a
gate-mass file — the exact distinction JustVugg pulled out of #1609:
counting picks degrades any saliency metric to selection frequency,
because a rare specialist with high gate mass is indistinguishable from
a rare marginal expert. Mass answers that.

Usage: parse_route_trace.py route_trace.txt --out usage.txt
       [--mass-out usage_mass.txt] [--dims-from config.json]
Header line `-1 n_layers n_experts` comes from --dims-from when given, else
from max observed indices (a lower bound — the script says so on stderr).
"""
import argparse, json, sys
from collections import Counter, defaultdict

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--out", default="usage.txt")
    ap.add_argument("--mass-out", default=None)
    ap.add_argument("--dims-from", default=None,
                    help="model config.json for exact n_layers/n_experts (else max seen)")
    a = ap.parse_args()

    dims = None
    if a.dims_from:
        c = json.load(open(a.dims_from))
        nl = c.get("num_hidden_layers")
        ne = (c.get("num_local_experts") or c.get("n_routed_experts")
              or c.get("num_experts"))
        if not (nl and ne):
            sys.exit(f"{a.dims_from}: no layer/expert count fields found")
        dims = (int(nl), int(ne))

    counts = defaultdict(Counter)   # layer -> Counter(expert -> picks)
    mass = defaultdict(dict)        # layer -> dict(expert -> summed gate)
    maxl = maxe = npicks = bad = 0
    for ln in open(a.trace):
        parts = ln.split()
        if len(parts) < 4:
            if parts: bad += 1
            continue
        layer = int(parts[2])
        for tok in parts[3:]:
            eid, _, gate = tok.partition(":")
            e = int(eid); g = float(gate or 0.0)
            counts[layer][e] += 1
            mass[layer][e] = mass[layer].get(e, 0.0) + g
            maxl = max(maxl, layer); maxe = max(maxe, e); npicks += 1
    if npicks == 0:
        sys.exit("no route rows parsed — is this the ROUTE_TRACE format?")
    if bad:
        print(f"note: {bad} unparsable lines skipped", file=sys.stderr)

    if dims is None:
        dims = (maxl + 1, maxe + 1)
        print(f"warning: dims inferred as {dims[0]}x{dims[1]} from max indices "
              f"(a lower bound; pass --dims-from for the real shape)", file=sys.stderr)
    nl, ne = dims
    if maxl >= nl or maxe >= ne:
        sys.exit(f"data exceeds declared dims {nl}x{ne} — wrong config for this trace?")

    def dump(path, fmt_value):
        with open(path, "w") as f:
            f.write(f"-1 {nl} {ne}\n-2 1 0\n")
            for l in sorted(counts):
                for e in sorted(counts[l]):
                    f.write(f"{l} {e} {fmt_value(l, e)}\n")

    dump(a.out, lambda l, e: counts[l][e])
    print(f"{npicks:,} picks -> {a.out} ({nl}x{ne})")
    if a.mass_out:
        dump(a.mass_out, lambda l, e: f"{mass[l][e]:.6f}")
        seen = sum(len(mass[l]) for l in mass)
        print(f"gate mass (sum of applied gate weights) -> {a.mass_out}")
        print(f"experts with traffic: {seen} of {nl*ne} ({100*seen/(nl*ne):.1f}%)")

if __name__ == "__main__":
    main()
