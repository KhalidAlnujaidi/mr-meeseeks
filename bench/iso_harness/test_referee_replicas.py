#!/usr/bin/env python3
"""test_referee_replicas.py — per-replica judging regression (F109/F114/F115).

Pins the referee behaviour that made the replica mechanism real:
 - a multi-replica out.json produces ONE JUDGED ROW PER REPLICA
 - each replica gets its own verdict from its OWN evidence
 - the old {task: row} collapse (last-row-wins) is what this replaces

Verified failing against the pre-F114 referee (see run log in the F114 entry).

Run: ~/.golem-iso-venv/bin/python test_referee_replicas.py
"""
import json
import os
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


tmp = Path(tempfile.mkdtemp())
sandbox = tmp / "sb"
sandbox.mkdir()
tasks = tmp / "tasks.json"
tasks.write_text(json.dumps({"tasks": [
    {"id": "T1", "category": "file_ops", "text": "make note.txt",
     "ground_truth": {"file": "note.txt", "equals": "OK"}}]}))
proxy = tmp / "proxy.jsonl"
proxy.write_text("")

# A three-replica out.json exactly as the golem runner now emits it:
# replica 0 never spawned, replica 1 produced the right artifact, replica 2
# never spawned. The OLD referee keyed {task: row} and judged only the LAST
# row (replica 2) -> one row, one verdict, two samples silently deleted.
rout = tmp / "rout.json"
rout.write_text(json.dumps({
    "harness": "golem", "peak_ram_mb": 8,
    "results": [
        {"task": "T1", "replica": 0, "wall_ms": 10, "spawns": 0, "gate_holds": 0,
         "exit_codes": [], "started_at_ms": 1, "finished_at_ms": 2,
         "files_created": [], "files_removed": []},
        {"task": "T1", "replica": 1, "wall_ms": 10, "spawns": 1, "gate_holds": 0,
         "exit_codes": [0], "started_at_ms": 3, "finished_at_ms": 4,
         "files_created": ["note.txt"], "files_removed": []},
        {"task": "T1", "replica": 2, "wall_ms": 10, "spawns": 0, "gate_holds": 0,
         "exit_codes": [], "started_at_ms": 5, "finished_at_ms": 6,
         "files_created": [], "files_removed": []},
    ]}))
(sandbox / "note.txt").write_text("OK\n")

jl = tmp / "out.jsonl"
env = dict(os.environ, ISO_RUN_BATCH="UNIT")
env.pop("ISO_REPS", None)
subprocess.run([sys.executable, str(HERE / "referee.py"), "golem", str(sandbox),
                str(tasks), str(rout), str(proxy), str(jl)],
               capture_output=True, text=True, env=env)
rows = [json.loads(l) for l in jl.read_text().splitlines() if l.strip()]

print("=== A. one judged row PER REPLICA (F114) ===")
check("A1 three replicas -> three judged rows (old code: 1)", len(rows), 3)
check("A2 replica indices preserved", [r["replica"] for r in rows], [0, 1, 2])

print()
print("=== B. each replica judged from its OWN evidence ===")
by = {r["replica"]: r for r in rows}
check("B1 replica 0 never spawned -> fail", by[0]["pass_f72"], False)
check("B2 replica 1 produced the artifact -> pass", by[1]["pass_f72"], True)
check("B3 replica 2 never spawned -> fail", by[2]["pass_f72"], False)
check("B4 replica 1 recorded its exit code", by[1]["exit_codes"], [0])
check("B5 replica 0 has no exit codes", by[0]["exit_codes"], [])

print()
print("=== C. batch stamping survives per-replica fan-out (F111) ===")
check("C1 every row carries the run batch", {r["run_batch"] for r in rows}, {"UNIT"})

print()
print(f"=== {passed} passed, {failed} failed ===")
sys.exit(1 if failed else 0)
