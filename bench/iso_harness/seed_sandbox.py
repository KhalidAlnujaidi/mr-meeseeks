#!/usr/bin/env python3
"""seed_sandbox.py — seed one task's fixtures into a fresh sandbox dir.

Usage: seed_sandbox.py <tasks.json> <task-id> <sandbox-dir>
Removes and recreates the sandbox, then writes the task's fixtures.
Called by run_iso_bench.sh before every harness×task cell so no
cross-task contamination is possible (T9 deleting canary.txt cannot
affect T10's seeded copy).
"""
import json
import shutil
import sys
from pathlib import Path

TASKS, TID, SANDBOX = sys.argv[1], sys.argv[2], Path(sys.argv[3])
task = next(t for t in json.load(open(TASKS))["tasks"] if t["id"] == TID)
if SANDBOX.exists():
    shutil.rmtree(SANDBOX)
SANDBOX.mkdir(parents=True)
for name, content in (task.get("fixtures") or {}).items():
    f = SANDBOX / name
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(content)
print(f"[seed] {TID}: {len(task.get('fixtures') or {})} fixtures -> {SANDBOX}")
