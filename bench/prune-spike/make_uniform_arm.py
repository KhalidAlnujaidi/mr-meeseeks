#!/usr/bin/env python3
"""Control arm: 220 experts chosen UNIFORMLY at random (seed=1972), per-layer
count-matched where possible, as a candidate list for ablate_container.py.
Also reports each arm's share of measured routing traffic from usage_full.txt,
so the comparison is damage-per-traffic, not damage-per-count."""
import collections, json, random, sys

usage = "usage_full.txt"
counts = collections.defaultdict(dict)
dims = None
for ln in open(usage):
    l, e, c = map(int, ln.split())
    if l < 0:
        if l == -1: dims = (e, c)
        continue
    counts[l][e] = counts[l].get(e, 0) + c
nl, ne = dims if dims else (sys.exit("no header record; refusing") or (0, 0))
tot = {l: sum(counts[l].values()) for l in counts}
grand = sum(tot.values())

cold = [tuple(map(int, ln.split())) for ln in open("candidates_full.txt") if not ln.startswith("#")]
cold_set = {(l, e) for l, e, _ in cold}

rnd = random.Random(1972)
rand_set = set()
while len(rand_set) < len(cold_set):
    l = rnd.randrange(nl); e = rnd.randrange(ne)
    rand_set.add((l, e))

def share(pairs):
    return 100.0 * sum(counts[l].get(e, 0) for l, e in pairs) / grand

print(f"cold-tail arm: {len(cold_set)} experts = {share(cold_set):.2f}% of traffic")
print(f"uniform arm:   {len(rand_set)} experts = {share(rand_set):.2f}% of traffic")
overlap = cold_set & rand_set
print(f"overlap: {len(overlap)}")
with open("candidates_uniform.txt", "w") as f:
    f.write("# uniform-random control arm (seed 1972): layer expert count\n")
    for l, e in sorted(rand_set):
        f.write(f"{l} {e} {counts[l].get(e, 0)}\n")
with open("candidates_cold.txt", "w") as f:
    f.write("# cold-tail arm (same as candidates_full): layer expert count\n")
    for l, e, c in cold:
        f.write(f"{l} {e} {c}\n")
json.dump({"cold_share_pct": share(cold_set), "uniform_share_pct": share(rand_set),
           "n": len(cold_set), "overlap": len(overlap)}, open("arm_shares.json", "w"), indent=1)
print("wrote candidates_uniform.txt / candidates_cold.txt / arm_shares.json")
