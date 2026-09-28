#!/usr/bin/env python3
"""Arm-C eval: cold-tail vs uniform-random ablation, same refs, same protocol.
Arms rotate within each (ref, rep) block; TF-NLL is deterministic so medians
equal any rep; kept for protocol symmetry with tier_eval.py."""
import json, os, re, statistics, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
OLMOE = os.path.join(HERE, "../../colibri/c/olmoe")
CONTAINERS = {
    "base": os.path.expanduser("~/models/olmoe_merged"),
    "cold": "/tmp/olmoe_tier220",
    "rand": "/tmp/olmoe_uniform",
}
REFS = ["eval_ref_1.json", "eval_ref_3.json"]        # ref2 duplicates ref1
REPS = 1                                              # deterministic NLL: reps add nothing
nll_re = re.compile(r"TF-NLL: ([\d.]+) nats/token over (\d+) tokens")

def run(container, ref):
    env = dict(os.environ, SNAP=container, PPL="1", OMP_NUM_THREADS="12")
    out = subprocess.run([OLMOE, "64", "8", os.path.join(HERE, ref)],
                         env=env, capture_output=True, text=True, timeout=600).stdout
    m = nll_re.search(out)
    if not m: raise RuntimeError("no TF-NLL:\n" + out[-300:])
    return float(m.group(1))

res = {}
arms = list(CONTAINERS)
for r in REFS:
    for rep in range(REPS):
        order = arms[rep % len(arms):] + arms[:rep % len(arms)]
        for a in order:
            v = run(CONTAINERS[a], r)
            res.setdefault(f"{a}|{r}", []).append(v)
            print(f"{r} {a:>4}: {v:.4f}", flush=True)

print()
for r in REFS:
    base = statistics.median(res[f"base|{r}"])
    cold = statistics.median(res[f"cold|{r}"])
    rnd = statistics.median(res[f"rand|{r}"])
    print(f"{r}: base {base:.4f} | cold-tail +{cold-base:.4f} | uniform +{rnd-base:.4f}")
json.dump(res, open(os.path.join(HERE, "armc_results.json"), "w"), indent=1)
