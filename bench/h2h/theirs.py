#!/usr/bin/env python3
"""theirs.py — the smolagents side of the head-to-head (bench/h2h).

FAIRNESS CONTRACT (bench/h2h/README.md F62-F67):
- Reads the SAME tasks.json as our side (F62: single source of truth).
- Talks to the SAME engine through the referee proxy (F63) — every
  wire fact is logged by proxy.py, never self-reported.
- Stock smolagents 1.26 CodeAgent + LocalPythonExecutor, documented
  default security config (F67). Only deviations: max_steps=6 (LOWER
  than their default 20 — conservative against them, bounds wall time
  on a disk-bound engine) and max_tokens=256 (parity with our side).
- Works inside sandbox-theirs/ with the canary pre-planted (F64).

Usage: theirs.py <proxy-base-url> <model-id> <sandbox-dir> <tasks.json> <out.json>
"""
import json
import os
import sys
import time

from smolagents import CodeAgent, LocalPythonExecutor, OpenAIServerModel

API_BASE, MODEL_ID, SANDBOX, TASKS, OUT = sys.argv[1:6]
# F69 (bench bug, smoke-caught): the OpenAI SDK appends /chat/completions
# to api_base itself — passing the full path 404s every request. Callers
# must pass the /v1 base only.
if API_BASE.endswith("/chat/completions"):
    API_BASE = API_BASE[: -len("/chat/completions")]

# F64: the agent only ever sees/works inside its sandbox.
os.chdir(SANDBOX)
# Parity cap (F67 documented deviation #2).
model = OpenAIServerModel(
    model_id=MODEL_ID,
    api_base=API_BASE,
    api_key="***",  # loopback; gateway ignores it
    max_tokens=256,
)
executor = LocalPythonExecutor(additional_authorized_imports=["os", "pathlib"])
agent = CodeAgent(tools=[], model=model, executor=executor, max_steps=6)

tasks = json.load(open(TASKS))["tasks"]
results = []
# F66 warmup parity: one throwaway completion through the referee proxy.
try:
    model([{"role": "user", "content": "Reply with the single word: warm"}])
except Exception as e:  # noqa: BLE001 — warmup failure is diagnostic only
    print(f"[warmup] failed: {e}", file=sys.stderr)

for t in tasks:
    prompt = (
        f"{t['text']}\n"
        "Work in the current directory. When done, call final_answer(\"done\")."
    )
    t0 = time.time()
    entry = {"task": t["id"], "wall_ms": None, "error": None, "final": None}
    try:
        out = agent.run(prompt, reset=True)
        entry["final"] = str(out)[:500]
    except Exception as e:  # noqa: BLE001 — recorded as evidence, not a crash
        entry["error"] = f"{type(e).__name__}: {e}"[:500]
    entry["wall_ms"] = int((time.time() - t0) * 1000)
    results.append(entry)
    print(f"[theirs] {t['id']}: wall_ms={entry['wall_ms']} "
          f"error={'yes' if entry['error'] else 'no'}")

json.dump({"harness": "smolagents-1.26-CodeAgent", "results": results},
          open(OUT, "w"), indent=1)
print(f"[theirs] wrote {OUT}")
