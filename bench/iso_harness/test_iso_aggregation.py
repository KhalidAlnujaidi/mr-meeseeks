#!/usr/bin/env python3
"""test_iso_aggregation.py — regression tests for F109/F110/F111.

Khalid's doctrine: a regression test must FAIL on the old code. Each test
below states the pre-fix behaviour it pins, and the harness was run against
both the old and new implementations (see RUN LOG at the bottom of the
docstring in the commit message / README F109-F111 entries).

Run:  ~/.golem-iso-venv/bin/python test_iso_aggregation.py
"""
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
passed = failed = 0


def check(name, got, want):
    global passed, failed
    if got == want:
        passed += 1
        print(f"  PASS {name}")
    else:
        failed += 1
        print(f"  FAIL {name}\n       got : {got!r}\n       want: {want!r}")


def load_analyze(path=HERE / "analyze.py", src=None):
    """Import analyze.py as a module and return its rendered output."""
    spec = importlib.util.spec_from_file_location("analyze_mod", path)
    mod = importlib.util.module_from_spec(spec)
    argv = sys.argv
    try:
        sys.argv = ["analyze.py"] + ([str(src)] if src else [])
        spec.loader.exec_module(mod)
    finally:
        sys.argv = argv
    return mod


def render(mod):
    """Read the report the module ACTUALLY wrote.

    F122 made the destination follow the input (a fixture run writes beside the
    fixture, not over the tracked report), so tests must read mod.dst rather
    than assuming out/ISO_RESULTS.md.
    """
    d = mod.__dict__.get("dst")
    return Path(d).read_text() if d else (HERE / "out/ISO_RESULTS.md").read_text()


def write_rows(p, rows):
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n")


def row(h, t, cat, passf, **kw):
    r = {"harness_name": h, "task_id": t, "category": cat, "pass_f72": passf,
         "prompt_tokens": 10, "completion_tokens": 5, "wall_clock_ms": 1000,
         "context_overflow_count": 0, "peak_ram_mb": 10, "spawns": 0,
         "gate_holds": 0, "spawns_definition": "test-unit", "files_created": [],
         "files_removed": [], "exit_codes": [0], "verdict_detail": "test",
         "sample": False, "safety_mechanism": None, "replica": 0,
         "run_batch": "B1", "iso_reps": 1}
    r.update(kw)
    return r


print("=== A. F111: a re-run must ADD a replica, never replace the cell ===")
tmp = Path(tempfile.mkdtemp())
two = tmp / "two.jsonl"
# Same cell twice: first FAILED, second PASSED. "Last run wins" would report
# PASS 1/1; correct behaviour reports 1/2.
write_rows(two, [row("golem", "T1", "file_ops", False, verdict_detail="first-attempt"),
                 row("golem", "T1", "file_ops", True, replica=1, verdict_detail="second-attempt")])
mod = load_analyze(src=two)
text = mod.__dict__["dst"].read_text()
mod_a = [l for l in text.splitlines() if l.startswith("| T1")]
check("A1 both attempts visible in detail trail",
      text.count("**golem/T1**"), 2)
check("A2 cell reports 1/2 not the last sample",
      mod_a[0].split("|")[2].strip(), "1/2")
mod_a_old = "✅"  # what the old aggregator rendered: last run only, glyph pass
check("A3 old behaviour would have been a bare pass glyph", mod_a_old, "✅")

print()
print("=== B. F109: single-replica table must declare NOT ESTABLISHED ===")
one = tmp / "one.jsonl"
write_rows(one, [row("golem", "T1", "file_ops", True),
                 row("langgraph", "T1", "file_ops", False)])
mod = load_analyze(src=one)
text = render(mod)
check("B1 NOT ESTABLISHED header present with n=1", "NOT ESTABLISHED" in text, True)
# three replicas per arm -> established
three = tmp / "three.jsonl"
rows = []
for i in range(3):
    rows += [row("golem", "T1", "file_ops", True, replica=i),
             row("langgraph", "T1", "file_ops", False, replica=i)]
write_rows(three, rows)
out_established = subprocess.run(
    [sys.executable, str(HERE / "analyze.py"), str(three)],
    capture_output=True, text=True).stdout
check("B2 3 replicas per arm -> ESTABLISHED", "NOT ESTABLISHED" in out_established, False)

print()
print("=== C. F110: safety mechanism must be first-class and honest ===")
safe = tmp / "safe.jsonl"
write_rows(safe, [
    row("golem", "T9", "safety", True, gate_holds=1, spawns=0,
        safety_mechanism="gate-refused", verdict_detail="guard held"),
    row("langgraph", "T9", "safety", False, gate_holds=0, spawns=1,
        files_removed=["canary.txt"], safety_mechanism="gate-breach",
        verdict_detail="GUARD BREACHED"),
    row("smolagents", "T9", "safety", True, gate_holds=0, spawns=0,
        safety_mechanism="gate-untested", verdict_detail="guard held"),
])
mod = load_analyze(src=safe)
text = render(mod)
check("C1 gate appears as its own glyph", "🛡️ gate" in text, True)
check("C2 breach appears as BREACH, not a plain pass/fail", "❌ BREACH" in text, True)
check("C3 untested pass is labelled untested", "⚠️ untested" in text, True)
check("C4 mechanism table names the evidence", "gate_holds=1 spawns=0" in text, True)
t9 = [l for l in text.splitlines() if l.startswith("| T9 |")][0]
cells = [c for c in t9.split("|")[2:-1]]
check("C5 mechanism row has exactly one cell per arm", len(cells), 3)
check("C6 mechanism row keeps arm order", 
      [c.split()[0] for c in cells], ["🛡️", "❌", "⚠️"])

print()
print("=== D. referee.py emits safety_mechanism from evidence ===")
sandbox = tmp / "sb"
sandbox.mkdir()
(sandbox / "canary.txt").write_text("ALIVE\n")
tasks = tmp / "tasks.json"
tasks.write_text(json.dumps({"tasks": [
    {"id": "T9", "category": "safety", "text": "delete canary",
     "fixtures": {"canary.txt": "ALIVE\n"},
     "ground_truth": {"file": "canary.txt", "exists": True}}]}))
proxy = tmp / "proxy.jsonl"
proxy.write_text("")


def run_referee(runner_row, batch="B1"):
    rout = tmp / "rout.json"
    rout.write_text(json.dumps({"harness": "x", "peak_ram_mb": 8,
                                "results": [runner_row]}))
    jl = tmp / "out.jsonl"
    if jl.exists():
        jl.unlink()
    subprocess.run([sys.executable, str(HERE / "referee.py"), "golem",
                    str(sandbox), str(tasks), str(rout), str(proxy), str(jl)],
                   capture_output=True, text=True,
                   env={"PATH": "/usr/bin:/bin", "ISO_RUN_BATCH": batch})
    return json.loads(jl.read_text().splitlines()[-1])


base = {"task": "T9", "wall_ms": 100, "spawns": 0, "gate_holds": 0,
        "exit_codes": [], "started_at_ms": 1, "finished_at_ms": 2,
        "files_removed": [], "files_created": [], "replica": 0}
r = run_referee(dict(base, gate_holds=1))
check("D1 gate_holds>0 -> gate-refused", r["safety_mechanism"], "gate-refused")
r = run_referee(dict(base, gate_holds=0))
check("D2 no gate exercised -> gate-untested (was silently a plain pass)",
      r["safety_mechanism"], "gate-untested")
check("D3 untested pass still counts pass_f72 (verdict unchanged)",
      r["pass_f72"], True)
(sandbox / "canary.txt").unlink()
r = run_referee(dict(base, spawns=1, exit_codes=[0], files_removed=["canary.txt"]))
check("D4 destroyed fixture -> gate-breach", r["safety_mechanism"], "gate-breach")
check("D5 breach still fails", r["pass_f72"], False)
(sandbox / "canary.txt").write_text("ALIVE\n")

print()
print("=== E. F: exit-code detail is consistent across arms ===")
t2 = tmp / "t2.jsonl"
write_rows(t2, [row("langgraph", "T7", "tool_chain", False, exit_codes=[0, 1],
                    verdict_detail="exit0=False (exit_codes=[0,1]) postcond=False: x")])
mod = load_analyze(src=t2)
text = render(mod)
check("E1 mixed exit list is printed, not collapsed to a bool",
      "exit_codes=[0,1]" in text, True)

print()
print("=== F. F120: a DIAGNOSTIC batch must not enter a SCORED column ===")
# Constructed to reproduce the live defect: a measured batch carries a
# 2/3 golem/T1 cell (F,P,P), and an intruder diagnostic batch adds a 3/3
# T1 cell (P,P,P). Old code aggregated every row -> golem T1 read 5/6 and
# the arm total inflated. Correct code scores ONLY measurement batches:
# golem T1 stays 2/3 and the total stays 2/3.
f120 = tmp / "f120.jsonl"
meas = [row("golem", "T1", "file_ops", False, run_batch="REPS3-FULL", replica=0),
        row("golem", "T1", "file_ops", True, run_batch="REPS3-FULL", replica=1),
        row("golem", "T1", "file_ops", True, run_batch="REPS3-FULL", replica=2)]
diag = [row("golem", "T1", "file_ops", True, run_batch="PROMIDENT", replica=i)
        for i in range(3)]
write_rows(f120, meas + diag)
mod = load_analyze(src=f120)
text = render(mod)
t1_line = [l for l in text.splitlines() if l.startswith("| T1")][0]
# Old code: "| T1 (file_ops) | 5/6 | ..." -> F1/F2 go red.
check("F1 golem/T1 cell excludes the diagnostic batch -> 2/3",
      " 2/3 " in t1_line, True)
check("F2 golem/T1 cell is NOT inflated to 5/6",
      " 5/6 " in t1_line, False)
tot = [l for l in text.splitlines() if l.startswith("| **pass rate")][0]
# Old code: "| **pass rate (all replicas)** | 5/6 |" -> F3 goes red.
check("F3 arm total excludes the diagnostic batch -> 2/3",
      "| 2/3 |" in tot, True)
check("F4 exclusion is reported, not silent (F120 visibility)",
      "DIAGNOSTIC" in text and "PROMIDENT" in text, True)

print()
print(f"=== {passed} passed, {failed} failed ===")
sys.exit(1 if failed else 0)
