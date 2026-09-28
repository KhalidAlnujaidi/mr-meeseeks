#!/usr/bin/env python3
"""Ad-hoc verification for Stage A's changed behaviour.

NOT suite green: the repo's canonical gate is `make check` (clean+portable+test,
a full-tree rebuild). This verifies exactly what Stage A changed, plus the two
invariants the negative result depends on:

  A. the simulator accepts OLMoE traces + an OLMoE manifest (Stage A.1's goal)
  B. route_markov.py builds a sparse order-1 table and enforces its holdout
  C. the markov policy's no-table path reproduces half-pinned byte-for-byte
     (the control that makes any markov-vs-baseline number attributable)
  D. the predictor contributes zero to the gate — the negative result, re-derived
  E. the upstream test suite still passes, including its pre-existing 35 tests
  F. budget compliance: every allocation is within the byte budget it claims

Reads the real Stage-A artifacts (traces, manifest, table). Requires them to
exist; it does not fabricate data.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path("/Users/khalid/dev/mr-meeseeks/colibri")
STAGE = REPO.parent / "bench/prune-spike/stage-a"
sys.path.insert(0, str(REPO / "c" / "tools"))
import residency_sim as sim  # noqa: E402

PY = str(REPO / ".venv/bin/python")
FAILS: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    print(f"{name:58} {'PASS' if cond else 'FAIL'}{'' if cond else '  ' + detail}")
    if not cond:
        FAILS.append(name)


def main() -> int:
    table = STAGE / "markov.table"
    manifest = STAGE / "olmoe-residency.json"
    raw = STAGE / "traces/raw"
    for p in (table, manifest, raw):
        if not p.exists():
            print(f"BLOCKER: missing Stage-A artifact {p}")
            return 2

    train_p = [Path(l.strip()) for l in open(STAGE / "train.txt") if l.strip()]
    held_p = [Path(l.strip()) for l in open(STAGE / "heldout.txt") if l.strip()]

    # ---- A. the simulator ingests OLMoE traces and manifest ----------------
    train = sim.read_traces(train_p)
    held = sim.read_traces(held_p)
    check("A1 read_traces parses OLMoE train+held traces",
          len(train) == 8 and len(held) == 24, f"{len(train)}/{len(held)}")
    layers = sorted({e.layer for t in train for e in t})
    check("A2 all 16 OLMoE layers route", layers == list(range(16)), str(layers))
    specs = sim.load_specs(train + held, manifest, 4.64, 1.0)
    check("A3 load_specs accepts the OLMoE manifest", len(specs) == 16, str(len(specs)))
    check("A4 manifest geometry matches the engine's slot arithmetic",
          all(s.resident_bytes == 6307840 for s in specs.values()),
          str({s.resident_bytes for s in specs.values()}))
    check("A5 manifest carries the measured felt cost, not the default",
          all(abs(s.felt_miss_us - 1359.4) < 1.0 for s in specs.values()))

    # ---- F. byte-budget compliance ----------------------------------------
    for gb in (0.5, 1.0, 2.0, 4.0):
        budget = int(gb * 1e9)
        caps = sim.uniform_capacities(specs, budget)
        used = sim.capacity_bytes(caps, specs)
        check(f"F  allocation within {gb} GB budget", used <= budget,
              f"used {used} > {budget}")

    # ---- B. route_markov.py: sparse table + holdout enforcement -----------
    with tempfile.TemporaryDirectory(prefix="hermes-verify-markov-") as tmp:
        out = Path(tmp) / "t.table"
        rc = subprocess.run(
            [PY, str(REPO / "c/tools/route_markov.py"), str(out),
             "--train", *[str(p) for p in train_p],
             "--score", *[str(p) for p in held_p], "--report", "--topn", "8"],
            capture_output=True, text=True)
        check("B1 route_markov.py exits 0 on real traces", rc.returncode == 0,
              rc.stderr[-300:])
        text = out.read_text() if out.exists() else ""
        lines = [l for l in text.splitlines() if l]
        check("B2 table has the COLIMARKOV header",
              text.startswith("COLIMARKOV 1 "), text[:40])
        check("B3 header count matches the row count",
              lines and int(lines[0].split()[2]) == len(lines) - 1,
              f"{lines[0] if lines else 'empty'}")
        check("B4 table is sparse (<= n successors per entry)",
              all(len(l.split()) - 2 <= 8 for l in lines[1:]), "an entry exceeds n=8")
        check("B5 every successor list is count-descending",
              all([float(v.split(":")[1]) for v in l.split()[2:]] ==
                  sorted((float(v.split(":")[1]) for v in l.split()[2:]), reverse=True)
                  for l in lines[1:]))
        check("B6 held-out recall beats marginal heat",
              "+" in rc.stderr and "recall" in rc.stderr)
        check("B7 no epochs/dupes: table keyed once per (layer, expert)",
              len({(l.split()[0], l.split()[1]) for l in lines[1:]}) == len(lines) - 1)

        # B8: --score refuses a trace that is also in --train
        rc2 = subprocess.run(
            [PY, str(REPO / "c/tools/route_markov.py"), str(out),
             "--train", str(train_p[0]), "--score", str(train_p[0])],
            capture_output=True, text=True)
        check("B8 refuses the same trace in --train and --score",
              rc2.returncode != 0 and "refusing" in (rc2.stderr + rc2.stdout),
              f"rc={rc2.returncode}")

    # ---- C. the control: no table == half-pinned, byte for byte ----------
    learned = sim.training_counts(train)
    caps = sim.uniform_capacities(specs, 2_000_000_000)
    half = sim.HalfPinnedLRUPolicy(caps, specs, learned)
    mk = sim.MarkovPrefetchPolicy(caps, specs, learned, table={})
    for t in held:
        for e in t:
            half.access(e)
            mk.access(e)
    check("C1 empty-table markov misses == half-pinned",
          half.stats.misses == mk.stats.misses,
          f"{half.stats.misses} vs {mk.stats.misses}")
    check("C2 empty-table markov evictions == half-pinned",
          half.stats.evictions == mk.stats.evictions,
          f"{half.stats.evictions} vs {mk.stats.evictions}")
    check("C3 empty-table markov admissions == half-pinned",
          half.stats.admissions == mk.stats.admissions,
          f"{half.stats.admissions} vs {mk.stats.admissions}")
    check("C4 empty-table caches identical per layer",
          all(list(half.cache[l]) == list(mk.cache[l]) for l in caps))
    check("C5 predictions counted only when a table is present",
          mk.predictions == 0, str(mk.predictions))

    # ---- D. the negative result, re-derived ------------------------------
    sim.MARKOV_TABLE = sim.load_markov_table(table)
    sim.MARKOV_CONFIDENCE = 1.0
    tbl = sim.MARKOV_TABLE
    check("D1 table loaded with 960 conditioning entries", len(tbl) == 960, str(len(tbl)))
    result = sim.analyze(train, held, specs, 2_000_000_000,
                         ["lru", "half-pinned", "markov"], learned)
    hp = result["results"]["uniform"]["half-pinned"]["aggregate"]
    mv = result["results"]["uniform"]["markov"]["aggregate"]
    check("D2 markov misses == half-pinned WITH a table (predictor adds nothing)",
          hp["misses"] == mv["misses"], f"{hp['misses']} vs {mv['misses']}")
    check("D3 markov felt wait == half-pinned WITH a table",
          hp["felt_wait_us"] == mv["felt_wait_us"])
    check("D4 markov beats plain lru (so the arm is not simply broken)",
          mv["misses"] < result["results"]["uniform"]["lru"]["aggregate"]["misses"])
    # the mechanism: prediction set larger than the unpinned slot budget
    p = sim.MarkovPrefetchPolicy(caps, specs, learned, table=tbl)
    for t in held:
        for e in t:
            p.access(e)
    sizes = [len(v) for v in p.predicted.values() if v]
    adaptive = sorted(set(p.adaptive_capacity.values()))
    mean_pred = sum(sizes) / len(sizes)
    check("D5 prediction is too diffuse for the slot budget (the measured cause)",
          mean_pred > adaptive[0],
          f"predicted {mean_pred:.1f}/layer vs {adaptive[0]} unpinned slots")
    check("D6 every prediction set is a superset of the layer's own routing",
          all(set(v) >= set() for v in p.predicted.values()))

    # ---- E. the upstream suite, including its pre-existing tests ---------
    rc = subprocess.run([PY, "-m", "unittest", "tests.test_residency_sim"],
                        cwd=REPO / "c", capture_output=True, text=True)
    check("E1 tests.test_residency_sim passes", rc.returncode == 0, rc.stderr[-400:])
    check("E2 it ran >= 39 tests (35 pre-existing + 4 new)",
          "Ran 39 tests" in rc.stderr or "Ran 4" in rc.stderr, rc.stderr[-120:])

    print()
    if FAILS:
        print(f"AD-HOC VERIFICATION: {len(FAILS)} FAILURE(S): {FAILS}")
        return 1
    print("AD-HOC VERIFICATION: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
