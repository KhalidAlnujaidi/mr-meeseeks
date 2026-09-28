#!/usr/bin/env python3
"""Paired TF-NLL eval: baseline vs zero-ablation tiers on 3 held-out refs.
Arms rotate order per (ref, repeat) block to decorrelate from drift; every
run is a fresh engine process. Reports per-arm medians and deltas."""
import json, os, re, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OLMOE = os.path.join(HERE, "../../colibri/c/olmoe")
CONTAINERS = {
    "base":  os.path.expanduser("~/models/olmoe_merged"),
    "t51":   "/tmp/olmoe_tier51",
    "t102":  "/tmp/olmoe_tier102",
    "t220":  "/tmp/olmoe_tier220",
}
REFS = ["eval_ref_1.json", "eval_ref_2.json", "eval_ref_3.json"]
REPS = 2
nll_re = re.compile(r"TF-NLL: ([\d.]+) nats/token over (\d+) tokens")

def run(container, ref):
    env = dict(os.environ, SNAP=container, PPL="1", OMP_NUM_THREADS="12")
    out = subprocess.run([OLMOE, "64", "8", os.path.join(HERE, ref)],
                         env=env, capture_output=True, text=True,
                         timeout=600).stdout
    m = nll_re.search(out)
    if not m: raise RuntimeError("no TF-NLL in:\n" + out[-300:])
    return float(m.group(1)), int(m.group(2))

results = {(a, r): [] for a in CONTAINERS for r in REFS}
arms = list(CONTAINERS)
for r in REFS:
    for rep in range(REPS):
        order = arms[rep % len(arms):] + arms[:rep % len(arms)]   # rotate
        for a in order:
            nll, n = run(CONTAINERS[a], r)
            results[(a, r)].append(nll)
            print(f"{r} rep{rep+1} {a:>4}: {nll:.4f} ({n} tok)", flush=True)

print("\n=== per-ref medians ===")
per_arm = {a: [] for a in arms}
for r in REFS:
    base = statistics.median(results[("base", r)])
    line = f"  {r}: " + " | ".join(
        f"{a} {statistics.median(results[(a,r)]):.4f} (+{statistics.median(results[(a,r)])-base:.4f})"
        for a in arms)
    print(line)
    for a in arms: per_arm[a].append(statistics.median(results[(a, r)]) - base)
print("\n=== delta vs baseline, mean over refs (nats/token of ~400 scored) ===")
for a in arms[1:]:
    print(f"  {a}: +{statistics.mean(per_arm[a]):.4f}  (per-ref: {[round(x,4) for x in per_arm[a]]})")
json.dump({f"{a}|{r}": v for (a, r), v in results.items()},
          open(os.path.join(HERE, "tier_eval_results.json"), "w"), indent=1)
