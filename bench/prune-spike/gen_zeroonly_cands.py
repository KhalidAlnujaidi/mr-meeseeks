#!/usr/bin/env python3
"""G3 corrected arms: zero-count-only candidates + matched-count uniform.

Motivation (from the 11:19 q38 run): the 0.1%-tail "cold" set at 48x512 was
99.9% low-traffic-BUT-ROUTED experts (12,328 of 14,022 had count>0). Their
zero-out can shift routing mass (ref_2 delta went NEGATIVE: -0.2086). To
answer the mechanism question we need the strict never-routed set compared
against a same-COUNT uniform-random arm, on a wider holdout (6 refs).

Writes into a FRESH work dir so run_experiment.py stays untouched:
  candidates_cold.txt  (the never-routed set; count column = 0)
  candidates_rand.txt  (same n, uniform over 48x512 grid, seed 1972)
  arm_shares.json      (traffic shares from usage)
"""
import argparse, collections, json, random

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("usage")
    ap.add_argument("--layers", type=int, default=48)
    ap.add_argument("--experts", type=int, default=512)
    ap.add_argument("--seed", type=int, default=1972)
    ap.add_argument("--outdir", required=True)
    a = ap.parse_args()

    counts = collections.defaultdict(dict)   # layer -> {expert: picks}
    totals = collections.defaultdict(int)
    for ln in open(a.usage):
        l, e, c = map(int, ln.split())
        if l < 0: continue
        counts[l][e] = counts[l].get(e, 0) + c
        totals[l] += c
    grand = sum(totals.values())

    zero = [(l, e) for l in range(a.layers) for e in range(a.experts)
            if e not in counts.get(l, {})]
    zset = set(zero)
    rnd = random.Random(a.seed)
    rand_s = set()
    while len(rand_s) < len(zero):
        rand_s.add((rnd.randrange(a.layers), rnd.randrange(a.experts)))
    rand = sorted(rand_s)

    share = lambda ps: 100.0 * sum(counts[l].get(e, 0) for l, e in ps) / grand
    import os
    os.makedirs(a.outdir, exist_ok=True)
    with open(os.path.join(a.outdir, "candidates_cold.txt"), "w") as f:
        f.write(f"# G3 zero-count-only: never routed in {a.usage.split('/')[-1]} "
                f"({len(zero)} of {a.layers*a.experts}); layer expert count\n")
        for l, e in zero: f.write(f"{l} {e} 0\n")
    with open(os.path.join(a.outdir, "candidates_rand.txt"), "w") as f:
        f.write(f"# G3 uniform-random matched count (seed {a.seed}): layer expert count\n")
        for l, e in rand: f.write(f"{l} {e} {counts[l].get(e, 0)}\n")
    arm = {"cold_share_pct": share(zero), "uniform_share_pct": share(rand),
           "n": len(zero), "overlap": len(zset & set(rand)),
           "grid": [a.layers, a.experts], "grand_picks": grand,
           "distinct_experts_touched": sum(len(v) for v in counts.values())}
    json.dump(arm, open(os.path.join(a.outdir, "arm_shares.json"), "w"), indent=1)
    print(f"zero-only n={len(zero)} (share {arm['cold_share_pct']:.4f}% of traffic)")
    print(f"matched rand n={len(rand)} (share {arm['uniform_share_pct']:.4f}%), overlap {arm['overlap']}")
    print(f"wrote candidates_cold.txt candidates_rand.txt arm_shares.json -> {a.outdir}")

if __name__ == "__main__":
    main()
