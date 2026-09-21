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

Usage: smolagents_runner.py <proxy-base-url> <model-id> <sandbox-dir>
                            <tasks.json> <out.json> <task-ids-csv>
F69: the OpenAI SDK appends /chat/completions — pass the /v1 base only.
"""
import json
import os
import resource
import shutil
import sys
import time

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
executor = LocalPythonExecutor(additional_authorized_imports=["os", "pathlib"])
agent = CodeAgent(tools=[], model=model, executor=executor, max_steps=6)

tasks = [t for t in json.load(open(TASKS))["tasks"] if t["id"] in WANTED]
os.chdir(SANDBOX)
results = []

# F66/F93 warmup parity: one throwaway completion through the referee
# proxy, only when the orchestrator asks (first invocation of the arm).
if os.environ.get("ISO_WARMUP") == "1":
    try:
        model([{"role": "user", "content": "Reply with the single word: warm"}])
    except Exception as e:  # noqa: BLE001 — diagnostic only
        print(f"[warmup] failed: {e}", file=sys.stderr)

for t in tasks:
    # Fixtures are seeded by the orchestrator into SANDBOX before each
    # task (fresh dir per harness×task, run_iso_bench.sh).
    prompt = (
        f"{t['text']}\n"
        'Work in the current directory. When done, call final_answer("done").'
    )
    t0 = time.time()
    started_ms = int(t0 * 1000)
    entry = {"task": t["id"], "wall_ms": None, "error": None, "final": None,
             "started_at_ms": started_ms, "spawns": 0, "gate_holds": 0}
    try:
        out = agent.run(prompt, reset=True)
        entry["final"] = str(out)[:500]
        entry["spawns"] = 1  # LocalPythonExecutor ran in-process (F71 delta)
    except Exception as e:  # noqa: BLE001 — recorded as evidence
        entry["error"] = f"{type(e).__name__}: {e}"[:500]
    entry["wall_ms"] = int((time.time() - t0) * 1000)
    entry["finished_at_ms"] = int(time.time() * 1000)
    results.append(entry)
    print(f"[smolagents-iso] {t['id']}: wall_ms={entry['wall_ms']} "
          f"error={'yes' if entry['error'] else 'no'}")

peak_mb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // (1024 * 1024)
json.dump({"harness": "smolagents", "peak_ram_mb": peak_mb,
           "results": results}, open(OUT, "w"), indent=1)
print(f"[smolagents-iso] wrote {OUT}")
