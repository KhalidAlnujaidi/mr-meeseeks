#!/usr/bin/env python3
"""smolagents_runner.py — smolagents adapter for bench/iso_harness.

Stock smolagents CodeAgent + LocalPythonExecutor (its documented
default loop, F91: no artificial JSON-payload parity — python code
execution IS smolagents' native contract). Documented deviations,
all conservative/neutral (F67 inheritance):
  - max_steps=6 (lower than their default 20; bounds wall time on a
    disk-bound engine — conservative against us AND them)
  - max_tokens=256, temperature=0.0 (iso protocol lock)
Works inside the per-task sandbox dir seeded by the orchestrator.

Verdicts are NOT made here (F89): writes a diagnostic out.json with
timing windows; referee.py judges from filesystem + proxy log.

F105 — `spawns` is MEASURED, not assumed. An earlier version set it to the
literal 1 ("the executor ran in-process"), and referee.py uses that number
to infer `executed_destructive` for the safety tasks, so a smolagents run
whose guard held perfectly was scored as a breach. This version counts
observable command executions the same way the langgraph arm does (each
`subprocess` the agent actually starts), plus records what happened to the
task's fixtures on disk so the referee has evidence rather than inference.

Usage: smolagents_runner.py <proxy-base-url> <model-id> <sandbox-dir>
                            <tasks.json> <out.json> <task-ids-csv>
F69: the OpenAI SDK appends /chat/completions — pass the /v1 base only.
"""
import json
import os
import resource
import subprocess
import sys
import time
from pathlib import Path

from smolagents import CodeAgent, LocalPythonExecutor, OpenAIServerModel

API_BASE, MODEL_ID, SANDBOX, TASKS, OUT, IDS = sys.argv[1:7]
if API_BASE.endswith("/chat/completions"):
    API_BASE = API_BASE[: -len("/chat/completions")]
WANTED = [x for x in IDS.split(",") if x]

model = OpenAIServerModel(
    model_id=MODEL_ID,
    api_base=API_BASE,
    api_key="***",  # loopback; engine ignores it
    max_tokens=256,
    temperature=0.0,  # F92
)

# F105: count the agent's actual command executions. LocalPythonExecutor
# runs in-process, so we count every call it is given, AND count any real
# subprocess the executed code spawns (a python snippet that shells out
# must not be invisible to the safety verdict).
_SPAWNED = {"n": 0}
_ORIG_SUBPROCESS_RUN = subprocess.run


def _counting_run(*a, **kw):
    _SPAWNED["n"] += 1
    return _ORIG_SUBPROCESS_RUN(*a, **kw)


subprocess.run = _counting_run  # visible to executed python


def _instrument_executor(ex):
    """Count each code block the executor actually runs."""
    orig = ex.__call__

    def counted(code, *a, **kw):
        _SPAWNED["n"] += 1
        return orig(code, *a, **kw)

    ex.__call__ = counted
    return ex


executor = _instrument_executor(
    LocalPythonExecutor(additional_authorized_imports=["os", "pathlib", "subprocess"])
)
agent = CodeAgent(tools=[], model=model, executor=executor, max_steps=6)

tasks = [t for t in json.load(open(TASKS))["tasks"] if t["id"] in WANTED]
os.chdir(SANDBOX)
SANDBOX = os.getcwd()
results = []

# F66/F93 warmup parity: one throwaway completion through the referee
# proxy, only when the orchestrator asks (first invocation of the arm).
if os.environ.get("ISO_WARMUP") == "1":
    try:
        model([{"role": "user", "content": "Reply with the single word: warm"}])
    except Exception as e:  # noqa: BLE001 — diagnostic only
        print(f"[warmup] failed: {e}", file=sys.stderr)


def snapshot():
    """Regular files in the sandbox, with sizes — for before/after diffing."""
    out = {}
    base = Path(SANDBOX)
    for p in base.rglob("*"):
        try:
            if p.is_file() and not p.is_symlink():
                out[str(p.relative_to(base))] = p.stat().st_size
        except OSError:
            continue
    return out


for t in tasks:
    # Fixtures are seeded by the orchestrator into SANDBOX before each
    # task (fresh dir per harness×task, run_iso_bench.sh).
    prompt = (
        f"{t['text']}\n"
        'Work in the current directory. When done, call final_answer("done").'
    )
    before = snapshot()
    _SPAWNED["n"] = 0
    t0 = time.time()
    started_ms = int(t0 * 1000)
    entry = {"task": t["id"], "wall_ms": None, "error": None, "final": None,
             "started_at_ms": started_ms, "spawns": 0, "gate_holds": 0,
             "exit_codes": [],
             # F105: name the axis so the three arms cannot be conflated.
             "spawns_definition": "in-process code executions (executor calls + shelled subprocesses)"}
    try:
        out = agent.run(prompt, reset=True)
        entry["final"] = str(out)[:500]
    except Exception as e:  # noqa: BLE001 — recorded as evidence
        entry["error"] = f"{type(e).__name__}: {e}"[:500]
    after = snapshot()

    # F105: measured, not assumed.
    entry["spawns"] = _SPAWNED["n"]

    # Evidence the referee's safety path can use instead of a spawn count:
    # which fixtures changed, and what new files appeared. The referee
    # still judges artifacts itself (F89) — this is corroboration.
    entry["fixtures_after"] = after
    entry["files_created"] = sorted(set(after) - set(before))
    entry["files_removed"] = sorted(set(before) - set(after))
    entry["files_changed"] = sorted(
        k for k in set(before) & set(after) if before[k] != after[k]
    )
    entry["wall_ms"] = int((time.time() - t0) * 1000)
    entry["finished_at_ms"] = int(time.time() * 1000)
    results.append(entry)
    print(f"[smolagents-iso] {t['id']}: wall_ms={entry['wall_ms']} "
          f"spawns={entry['spawns']} "
          f"created={entry['files_created']} removed={entry['files_removed']} "
          f"error={'yes' if entry['error'] else 'no'}")

peak_mb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // (1024 * 1024)
json.dump({"harness": "smolagents", "peak_ram_mb": peak_mb,
           "results": results}, open(OUT, "w"), indent=1)
print(f"[smolagents-iso] wrote {OUT}")
