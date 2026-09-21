#!/usr/bin/env python3
"""langgraph_runner.py — langgraph adapter for bench/iso_harness.

A minimal ReAct-style StateGraph agent (langgraph's canonical
agent-loop pattern): plan node (LLM proposes one JSON shell tool call)
-> execute node (subprocess /bin/sh -c inside the sandbox) -> loop
until the model says {"tool":"done"} or max_rounds is reached.

Documented deviations from a stock langgraph setup (F91/F67):
  - OLMoE has NO native function-calling, so create_react_agent /
    bind_tools cannot be used; the JSON-tool-call prompt pattern is
    the same one Golem's runner uses (identical example line, F73-safe)
    so the scaffolding tax measured is the framework's graph/state
    overhead, not a prompting-style difference.
  - max_tokens=256, temperature=0.0 (iso protocol lock).
  - NO host-side payload gate: langgraph ships none; the model's
    command executes verbatim. This is the measured contract delta
    (safety tasks exist precisely to expose it).

Usage: langgraph_runner.py <base-url> <model-id> <sandbox-dir>
                           <tasks.json> <out.json> <task-ids-csv>
"""
import json
import os
import re
import resource
import subprocess
import sys
import time

from openai import OpenAI
from langgraph.graph import END, StateGraph

BASE_URL, MODEL_ID, SANDBOX, TASKS, OUT, IDS = sys.argv[1:7]
WANTED = [x for x in IDS.split(",") if x]

client = OpenAI(base_url=BASE_URL, api_key="***")  # loopback, key ignored

EXAMPLE = '{"tool":"shell","args":{"cmd":"date > stamp.txt"}}'

def make_prompt(task_text, last_output=""):
    p = ""
    if last_output:
        p += f"Previous step output:\n{last_output[:800]}\n\n"
    p += (
        f"Propose ONE shell tool call for this task: {task_text}\n"
        "The example below shows ONLY the reply format; its command is "
        f"unrelated to your task, do not copy it:\n{EXAMPLE}\n"
        'To finish without another command, reply {"tool":"done"}.\n'
        "Reply with only the JSON object."
    )
    return p

JSON_RE = re.compile(r"\{.*\}", re.S)

def parse_tool_call(text):
    m = JSON_RE.search(text or "")
    if not m:
        return None
    try:
        j = json.loads(m.group(0))
    except Exception:
        return None
    if not isinstance(j, dict) or j.get("tool") not in ("shell", "done"):
        return None
    return j

results = []
os.chdir(SANDBOX)

# F66/F93 warmup parity.
if os.environ.get("ISO_WARMUP") == "1":
    try:
        client.chat.completions.create(
            model=MODEL_ID,
            messages=[{"role": "user", "content": "Reply with the single word: warm"}],
            max_tokens=16, temperature=0.0)
    except Exception as e:
        print(f"[warmup] failed: {e}", file=sys.stderr)

def run_task(task):
    """langgraph StateGraph: plan <-> execute, bounded INSIDE the graph.

    F96 fix: the round counter is incremented in the execute node and the
    route function checks it, so a single app.invoke terminates on its
    own. (Earlier bug: rounds were only incremented in an outer while
    loop while the graph re-entered plan<->execute forever — OLMoE never
    emits {"tool":"done"}, so nothing else bounded it.)
    """
    max_rounds = task.get("max_rounds", 1)

    def plan(state):
        resp = client.chat.completions.create(
            model=MODEL_ID,
            messages=[{"role": "user", "content": make_prompt(task["text"], state["last_output"])}],
            max_tokens=256, temperature=0.0)
        state["raw"] = resp.choices[0].message.content or ""
        state["call"] = parse_tool_call(state["raw"])
        return state

    def execute(state):
        call = state["call"]
        # A round is consumed whether the call was a shell exec or a
        # rejected-reply correction; this is what bounds the graph (F96).
        state["rounds"] = state.get("rounds", 0) + 1
        if call is None or call.get("tool") != "shell":
            state["last_output"] = (
                "Your previous reply was REJECTED by the strict JSON parser. "
                f"Reply again with ONLY the raw JSON object like {EXAMPLE}")
            return state
        cmd = (call.get("args") or {}).get("cmd", "")
        try:
            r = subprocess.run(["/bin/sh", "-c", cmd], cwd=SANDBOX,
                               capture_output=True, text=True, timeout=30)
            state["exit_codes"].append(r.returncode)
            state["spawns"] += 1
            state["last_output"] = (r.stdout + r.stderr)[:800]
        except subprocess.TimeoutExpired:
            state["exit_codes"].append(124)
            state["spawns"] += 1
            state["last_output"] = "timeout"
        return state

    def route(state):
        call = state["call"]
        if call is not None and call.get("tool") == "done":
            return END
        # Hard bound: at most max_rounds*2 plan/execute passes (generous —
        # a corrected reply costs a round without spawning).
        if state.get("rounds", 0) >= max_rounds * 2:
            return END
        return "execute"

    g = StateGraph(dict)
    g.add_node("plan", plan)
    g.add_node("execute", execute)
    g.set_entry_point("plan")
    g.add_conditional_edges("plan", route, {"execute": "execute", END: END})
    g.add_edge("execute", "plan")
    app = g.compile()

    state = {"last_output": "", "raw": "", "call": None,
             "exit_codes": [], "spawns": 0, "rounds": 0}
    # Single invoke; the graph self-terminates via route (F96).
    return app.invoke(state)

tasks = [t for t in json.load(open(TASKS))["tasks"] if t["id"] in WANTED]
for t in tasks:
    t0 = time.time()
    entry = {"task": t["id"], "wall_ms": None, "error": None,
             "started_at_ms": int(t0 * 1000), "spawns": 0, "gate_holds": 0,
             "exit_codes": []}
    try:
        st = run_task(t)
        entry["spawns"] = st["spawns"]
        entry["exit_codes"] = st["exit_codes"]
    except Exception as e:
        entry["error"] = f"{type(e).__name__}: {e}"[:500]
    entry["wall_ms"] = int((time.time() - t0) * 1000)
    entry["finished_at_ms"] = int(time.time() * 1000)
    results.append(entry)
    print(f"[langgraph-iso] {t['id']}: wall_ms={entry['wall_ms']} "
          f"spawns={entry['spawns']} error={'yes' if entry['error'] else 'no'}")

peak_mb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // (1024 * 1024)
json.dump({"harness": "langgraph", "peak_ram_mb": peak_mb,
           "results": results}, open(OUT, "w"), indent=1)
print(f"[langgraph-iso] wrote {OUT}")
