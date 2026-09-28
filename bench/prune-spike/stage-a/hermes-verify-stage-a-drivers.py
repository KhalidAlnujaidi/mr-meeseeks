#!/usr/bin/env python3
"""Ad-hoc verification for the Stage-A scripts not yet covered.

The sibling hermes-verify-stage-a.py (kept in stage-a/) already covers the
simulator, the markov policy, route_markov.py and the empty-table control.
This one closes the gap on the three driver scripts, none of which had been
exercised by a check:

  G  run_olmoe_gate.py      — reproduces the negative result, asserts budget
                              compliance, and asserts markov == half-pinned
  H  validate_felt_cost.py  — parses real engine output and reports the
                              non-constant cost per miss
  I  capture_traces.sh      — argument handling and the skip-existing path
  J  stage_a_build_corpus.py— the split/quota logic and its assertions

NOT suite green. The repo gate is `make check` (full-tree rebuild), not run.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path("/Users/khalid/dev/mr-meeseeks/colibri")
STAGE = REPO.parent / "bench/prune-spike/stage-a"
PY = str(REPO / ".venv/bin/python")
BIN = Path("/tmp/olmoe_pr1")
REF = Path("/Users/khalid/dev/mr-meeseeks/bench/prune-spike/eval_ref_1.json")
FAILS: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    print(f"{name:58} {'PASS' if cond else 'FAIL'}{'' if cond else '  ' + detail}")
    if not cond:
        FAILS.append(name)


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="hermes-verify-stage-a-drivers-"))
    try:
        return run(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(tmp: Path) -> int:
    # ---- G. run_olmoe_gate.py reproduces the negative result --------------
    out = tmp / "gate.json"
    r = subprocess.run(
        [PY, str(STAGE / "run_olmoe_gate.py"), "--traces", str(STAGE / "traces/raw"),
         "--manifest", str(STAGE / "olmoe-residency.json"),
         "--markov-table", str(STAGE / "markov.table"),
         "--budgets-gb", "1", "2", "4", "--read-gbps", "4.64",
         "--json-out", str(out)],
        cwd=STAGE, capture_output=True, text=True)
    check("G1 run_olmoe_gate.py exits 0", r.returncode == 0, r.stderr[-400:])
    check("G2 it wrote its JSON report", out.exists())
    if not out.exists():
        return finish()
    doc = json.loads(out.read_text())
    check("G3 three budgets reported", len(doc["budgets"]) == 3, str(len(doc["budgets"])))
    check("G4 four categories, by-session split",
          len(set(doc["categories"])) == 4 and len(doc["categories"]) == 24,
          f"{len(doc['categories'])} traces, {len(set(doc['categories']))} cats")
    for b in doc["budgets"]:
        gb, pols = b["budget_gb"], b["policies"]
        check(f"G5 [{gb}GB] allocation within budget",
              b["resident_bytes"] <= int(gb * 1e9),
              f"{b['resident_bytes']} > {int(gb*1e9)}")
        # the negative result, asserted rather than remembered
        check(f"G6 [{gb}GB] markov == half-pinned (predictor adds nothing)",
              pols["markov"]["misses"] == pols["half-pinned"]["misses"]
              and pols["markov"]["felt_wait_s"] == pols["half-pinned"]["felt_wait_s"],
              f"{pols['markov']} vs {pols['half-pinned']}")
        check(f"G7 [{gb}GB] markov beats lru (arm not broken)",
              pols["markov"]["misses"] < pols["lru"]["misses"])
        check(f"G8 [{gb}GB] every policy's bytes_read is miss*expert_size",
              all(abs(p["bytes_read"] - p["misses"] * 6307840) == 0
                  for p in pols.values()),
              "byte accounting drifts from the manifest")
    # the gate's own verdict must be the baseline's, not the predictor's
    marks = [c for b in doc["budgets"] for c in b["gate"]["candidates"]
             if c["policy"] == "markov"]
    halves = [c for b in doc["budgets"] for c in b["gate"]["candidates"]
              if c["policy"] == "half-pinned"]
    check("G9 markov gate verdicts match half-pinned's",
          [c["pass"] for c in marks] == [c["pass"] for c in halves])

    # ---- H. validate_felt_cost.py parses real engine output ---------------
    if BIN.exists() and REF.exists():
        r = subprocess.run(
            [PY, str(STAGE / "validate_felt_cost.py"), str(BIN), str(REF),
             "16", "48"],
            capture_output=True, text=True, cwd=STAGE)
        check("H1 validate_felt_cost.py exits 0", r.returncode == 0, r.stderr[-300:])
        check("H2 it parsed hit% and tok/s from the engine",
              "hit%" in r.stdout and "tok/s" in r.stdout, r.stdout[:200])
        check("H3 it reported the linear fit",
              "linear fit" in r.stdout and "RMS residual" in r.stdout)
        check("H4 PPL is reported stable across caps",
              "token-exact" in r.stdout or "CHANGED" in r.stdout)
        # µs/miss is the 6th column of the table; anchoring on the trailing NLL
        # column (as the first version of this check did) collects 3.837 twice
        # and asserts a false failure.
        rows = [l.split() for l in r.stdout.splitlines()
                if re.match(r"^\s*\d+\s+\d+\.\d+\s+\d+\s", l)]
        us = [float(row[5]) for row in rows if len(row) >= 6]
        check("H5 the table parsed two caps", len(us) == 2, str(rows))
        check("H6 cost per miss is not constant (the finding)",
              len(set(us)) > 1, str(us))
        check("H7 cost per miss rises with residency",
              len(us) == 2 and us[1] > us[0], str(us))
    else:
        print("H  SKIPPED (needs the engine binary and eval ref)")

    # ---- I. capture_traces.sh argument handling ---------------------------
    sh = STAGE / "capture_traces.sh"
    r = subprocess.run(["bash", str(sh)], capture_output=True, text=True, cwd=tmp)
    check("I1 capture_traces.sh refuses missing args",
          r.returncode != 0 and "binary" in (r.stderr + r.stdout).lower(),
          f"rc={r.returncode}")
    # skip-existing path: a pre-seeded trace must not be re-captured
    refs = tmp / "refs"; traces = tmp / "traces"
    refs.mkdir(); traces.mkdir()
    (refs / "only.json").write_text(json.dumps({"prompt_ids": [1], "full_ids": [1, 2]}))
    (traces / "only.trace").write_text("0 0 0 5:0.5\n")
    r = subprocess.run(["bash", str(sh), "/bin/false", str(refs), str(traces)],
                       capture_output=True, text=True, cwd=tmp)
    check("I2 existing trace is skipped, not re-run",
          "skip only" in r.stdout and (traces / "only.trace").read_text() == "0 0 0 5:0.5\n",
          r.stdout[:200])

    # ---- J. stage_a_build_corpus.py split logic ---------------------------
    src = (REPO / "c/tools/stage_a_build_corpus.py").read_text()
    check("J1 boilerplate rejection exists (the duplicate-content bug)",
          "is_boilerplate" in src and "hindsight_knowledge" in src)
    check("J2 duplicate-content guard aborts the build",
          "DUPLICATE CONTENT" in src and "SystemExit" in src)
    check("J3 per-split quota, not draw order (the 0-held-out bug)",
          '"train": max(1, a.per_category // 4)' in src)
    check("J4 session-sharing assertion present",
          "session shared between train and held-out" in src)
    check("J5 a --help style smoke runs without a model",
          subprocess.run([PY, str(REPO / "c/tools/stage_a_build_corpus.py"), "--help"],
                         capture_output=True).returncode == 0)
    return finish()


def finish() -> int:
    print()
    if FAILS:
        print(f"AD-HOC VERIFICATION: {len(FAILS)} FAILURE(S): {FAILS}")
        return 1
    print("AD-HOC VERIFICATION: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
