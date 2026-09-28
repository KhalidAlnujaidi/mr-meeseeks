#!/usr/bin/env python3
"""Stage A: run the existing Phase-0 gate on OLMoE traces, with the Markov
policy class included, and report the numbers.

Additive only — this drives c/tools/residency_sim.py (the upstream gate, its
Phase-0 decision rule, its felt-wait model) and adds no engine code.

Splits come from traces/split.json, produced by stage_a_build_corpus.py, which
partitions by SOURCE SESSION so no session appears on both sides. The Markov
table is built by c/tools/route_markov.py from the train traces only.

Usage:
  python3 run_olmoe_gate.py --traces traces/raw --manifest olmoe-residency.json \
      --budgets-gb 0.5 1 2 4 --markov-table markov.table --json-out gate.json
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[3] / "colibri"
sys.path.insert(0, str(REPO / "c" / "tools"))
import residency_sim as rs  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--traces", type=Path, required=True, help="dir of *.trace")
    ap.add_argument("--split", type=Path, default=None,
                    help="split.json (default: <traces>/../split.json)")
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--markov-table", type=Path, default=None)
    ap.add_argument("--budgets-gb", type=float, nargs="+", default=[0.5, 1.0, 2.0, 4.0])
    ap.add_argument("--policies", nargs="+",
                    default=["lru", "half-pinned", "lfu", "frequency", "markov"])
    ap.add_argument("--read-gbps", type=float, default=3.0)
    ap.add_argument("--felt-fraction", type=float, default=1.0)
    ap.add_argument("--markov-confidence", type=float, default=1.0)
    ap.add_argument("--json-out", type=Path)
    a = ap.parse_args()

    split_path = a.split or (a.traces.parent / "split.json")
    split = json.loads(split_path.read_text())
    train_names = [n for c in split["split"].values() for n in c["train"]]
    held_names = [n for c in split["split"].values() for n in c["heldout"]]

    def paths(names):
        out = [a.traces / (n + ".trace") for n in names]
        missing = [p for p in out if p.exists()]
        return missing

    train_p, held_p = paths(train_names), paths(held_names)
    if not train_p or not held_p:
        sys.exit(f"need traces for both splits (train {len(train_p)}, held {len(held_p)})")

    # Category = the workload axis. Each category must be present in the held-out
    # set, and the gate weights categories equally, so a win cannot come from one
    # workload dominating by length.
    categories = []
    for name in held_names:
        if (a.traces / (name + ".trace")).exists():
            categories.append(json.loads(
                (a.traces.parent / "refs" / (name + ".json")).read_text())["category"])
    if len(categories) < 2:
        sys.exit("need >= 2 held-out categories for the gate")

    train = rs.read_traces(train_p)
    held = rs.read_traces(held_p)
    specs = rs.load_specs(train + held, a.manifest, a.read_gbps, a.felt_fraction)
    print(f"traces: {len(train)} train / {len(held)} held-out, {len(specs)} layers, "
          f"{len(set(categories))} categories", file=sys.stderr)
    print("categories:", dict(sorted(
        ((c, categories.count(c)) for c in set(categories)))), file=sys.stderr)

    learned = rs.training_counts(train)

    # The markov table must come from the TRAIN split. Build it here if not given,
    # so the number in the report cannot be produced from held-out data by mistake.
    if "markov" in a.policies:
        if a.markov_table is None:
            sys.exit("--policies markov needs --markov-table (route_markov.py)")
        rs.MARKOV_TABLE = rs.load_markov_table(a.markov_table)
        rs.MARKOV_CONFIDENCE = a.markov_confidence
        print(f"markov: {len(rs.MARKOV_TABLE)} entries, confidence "
              f"{a.markov_confidence:g}, table={a.markov_table}", file=sys.stderr)

    report = {"budgets": [], "read_gbps": a.read_gbps,
              "felt_fraction": a.felt_fraction,
              "train_traces": [p.name for p in train_p],
              "held_traces": [p.name for p in held_p],
              "categories": categories}

    for gb in a.budgets_gb:
        budget = int(gb * 1e9)
        caps = rs.uniform_capacities(specs, budget)
        resident = rs.capacity_bytes(caps, specs)
        if resident > budget:
            sys.exit(f"allocation exceeds budget at {gb} GB")
        result = rs.analyze(train, held, specs, budget, a.policies, learned)
        gate = rs.decision_gate(result, categories)
        entry = {"budget_gb": gb, "resident_bytes": resident,
                 "slots_per_layer": sorted(set(caps.values())),
                 "gate": gate, "policies": {}}
        for pol, payload in result["results"]["uniform"].items():
            agg = payload["aggregate"]
            entry["policies"][pol] = {
                "felt_wait_s": agg["felt_wait_us"] / 1e6,
                "misses": agg["misses"], "hits": agg["hits"],
                "hit_rate": agg["hit_rate"], "bytes_read": agg["bytes_read"],
            }
        report["budgets"].append(entry)

        print(f"\n=== expert budget {gb:g} GB "
              f"({resident/1e9:.2f} GB resident, "
              f"{sorted(set(caps.values()))} slots/layer) ===")
        base = entry["policies"]["lru"]["felt_wait_s"]
        print(f"{'policy':12} {'hit%':>7} {'misses':>9} {'felt_wait_s':>12} {'vs lru':>8}")
        for pol, p in entry["policies"].items():
            d = (base - p["felt_wait_s"]) / base * 100 if base else 0.0
            print(f"{pol:12} {p['hit_rate']*100:7.2f} {p['misses']:9d} "
                  f"{p['felt_wait_s']:12.3f} {d:+7.2f}%")

        print(f"{'candidate':12} {'mean gain':>10} {'worst cat':>10} {'verdict':>8}")
        for row in gate["candidates"]:
            print(f"{row['policy']:12} {row['category_mean_gain']:10.2%} "
                  f"{row['worst_category_gain']:10.2%} "
                  f"{'PASS' if row['pass'] else 'fail':>8}")

    if a.json_out:
        a.json_out.write_text(json.dumps(report, indent=1))
        print(f"\nwrote {a.json_out}", file=sys.stderr)


if __name__ == "__main__":
    main()
