#!/usr/bin/env python3
"""Paired cap sweep: main(a8f2ca6) vs PR1611 engine, OLMoE int8, PPL=1.
Acceptance per #1050: TF-NLL bit-identical across arms; tok/s must not fall as cap rises.
Protocol: one fresh process per run, arms interleaved within each cap block, median of REPS.
"""
import subprocess, statistics, sys, json, os

BASE = "/Users/khalid/dev/mr-meeseeks/colibri/c/olmoe"          # main @ a8f2ca6
PR   = "/tmp/colibri-pr1611/c/olmoe"                            # PR #1611 branch
REF  = "/Users/khalid/dev/mr-meeseeks/colibri/ref.json"
MODEL = os.path.expanduser("~/models/olmoe_merged")
CAPS = [4, 8, 16, 32, 64]
REPS = 3

def run(engine, cap):
    env = dict(os.environ, SNAP=MODEL, PPL="1", OMP_NUM_THREADS="12")
    out = subprocess.run([engine, str(cap), "8", REF], env=env,
                         capture_output=True, text=True, timeout=300).stdout
    nll = tok = hit = None
    for ln in out.splitlines():
        if ln.startswith("TF-NLL:"):
            nll = ln.split()[1]                      # bit-identical token, compare as string
        if "tok/s" in ln:
            tok = float(ln.split()[1])
        if "hit rate:" in ln:
            hit = ln.split("hit rate:")[1].split("%")[0].strip()
    if nll is None or tok is None:
        raise RuntimeError("parse fail:\n" + out)
    return nll, tok, hit

rows = {}
last_nll = ""
for cap in CAPS:
    a, b = [], []
    for _ in range(REPS):                            # interleaved arms, same cap block
        n1, t1, h1 = run(BASE, cap); a.append(t1)
        n2, t2, h2 = run(PR,   cap); b.append(t2)
        assert n1 == n2, f"NLL divergence cap={cap}: {n1} vs {n2}"
        last_nll = n1
    rows[cap] = dict(main=statistics.median(a), pr=statistics.median(b),
                     main_all=a, pr_all=b, nll=last_nll)
    print(f"cap {cap:>2} | NLL {last_nll} | main {statistics.median(a):5.2f} tok/s | PR {statistics.median(b):5.2f} tok/s", flush=True)

def monotonic(key):
    v = [rows[c][key] for c in CAPS]
    bad = [(c, i) for i in range(1, len(v)) if v[i] < v[i-1] * 0.95 for c in [CAPS[i]]]
    return v, bad

vm, bm = monotonic("main"); vp, bp = monotonic("pr")
print("\nmain tok/s by cap:", ["%.2f" % x for x in vm], "violations:", bm or "none")
print("PR   tok/s by cap:", ["%.2f" % x for x in vp], "violations:", bp or "none")
json.dump({str(k): v for k, v in rows.items()}, open("/tmp/colibri-cap-sweep.json", "w"), indent=1)
print("\nsaved /tmp/colibri-cap-sweep.json")
