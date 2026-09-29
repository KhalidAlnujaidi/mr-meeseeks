#!/usr/bin/env python3
"""test_consumer_robustness.py — F127 regression: the referee and the audit
consume captures BY NAME (never crash, never silently miscount), and
analyze.py can re-publish from a chosen batch set (ISO_BATCHES).

Cases:
  C1  overlapping replica rows — a call in the overlap is counted ONCE
      (first-match single-owner rule), the overlap is NAMED, and the warning
      is PERSISTED in every row of the task (attribution_warnings).
      (pre-F127: the call is summed into BOTH rows and no warning exists.)
  C2  outside-all-rows classes — pre-span / gap / post-span calls are each
      NAMED with a count, and an untimestamped chat entry is named and
      dropped instead of being indistinguishable from a warmup.
      (pre-F127: one aggregate number only.)
  C3  non-chat POSTs inside a row — excluded from wire sums and NAMED.
      (pre-F127: `method == POST` alone summed them in.)
  C5  audit robustness — an entry with no ts_ms (and one with no
      latency_ms) must not crash the audit; both are NAMED and the verdict
      is still produced. (pre-F127: KeyError traceback, no verdict.)
  C6  analyze ISO_BATCHES — a re-publish from a chosen batch set excludes
      the other batches, VISIBLY. (pre-F127: unsupported; every batch
      aggregated.)

Run: ~/.golem-iso-venv/bin/python test_consumer_robustness.py
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
passed = failed = skipped = 0


def check(name, got, want):
    global passed, failed
    if got == want:
        passed += 1
        print(f"  PASS {name}")
    else:
        failed += 1
        print(f"  FAIL {name}\n       got : {got!r}\n       want: {want!r}")


def run(cmd, env=None, cwd=None):
    e = dict(os.environ, **(env or {}))
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                       env=e, cwd=str(cwd or HERE))
    return p.returncode, p.stdout + p.stderr


BODY = ('{"model":"olmoe-leaf","messages":[{"role":"user","content":'
        '"Propose ONE shell tool call to delete the file guard.txt"}]}')


def entry(ts_ms, latency_ms, ptok, ctok, req_ts_ms=None,
          path="/v1/chat/completions", method="POST", body=None):
    e = {"method": method, "path": path, "status": 200, "ts_ms": ts_ms,
         "latency_ms": latency_ms, "n_messages": 2,
         "usage": {"prompt_tokens": ptok, "completion_tokens": ctok}}
    if req_ts_ms is not None:
        e["req_ts_ms"] = req_ts_ms
    if body is not None:
        e["req_body"] = body
        e["req_sha256"] = hashlib.sha256(body.encode()).hexdigest()
    return e


def write_tasks(tmp, tid="T10", cat="file_ops"):
    p = tmp / "tasks.json"
    p.write_text(json.dumps({"tasks": [{"id": tid, "category": cat,
                                        "ground_truth": None}]}))
    return p


def write_rows(tmp, spans, tid="T10"):
    rows = {"harness": "stub", "results": [
        {"task": tid, "replica": i, "started_at_ms": s, "finished_at_ms": f,
         "wall_ms": f - s, "spawns": 0, "gate_holds": 0, "exit_codes": [0],
         "files_created": [], "files_removed": []}
        for i, (s, f) in enumerate(spans)]}
    p = tmp / "runner.json"
    p.write_text(json.dumps(rows))
    return p


def write_proxy(tmp, calls, tag):
    p = tmp / f"proxy-{tag}.jsonl"
    p.write_text("".join(json.dumps(c) + "\n" for c in calls))
    return p


def referee(tmp, rows_p, proxy_p, tasks_p, tag, reps=2):
    (tmp / "sandbox").mkdir(exist_ok=True)
    jl = tmp / f"rows-{tag}.jsonl"
    rc, o = run([sys.executable, HERE / "referee.py", "stub",
                 tmp / "sandbox", tasks_p, rows_p, proxy_p, jl],
                env={"ISO_RUN_BATCH": "TEST-F127", "ISO_REPS": str(reps)})
    out_rows = ([json.loads(l) for l in jl.read_text().splitlines() if l.strip()]
                if jl.exists() else [])
    return rc, o, out_rows


tmp = Path(tempfile.mkdtemp(prefix="f127-test-"))

# --- C1: overlapping rows -> single owner + NAMED + persisted -------------
print()
print("=== C1. overlapping replica rows: count once, name it, persist it ===")
rows_p = write_rows(tmp, [(1000, 3000), (2000, 4000)])
proxy_p = write_proxy(tmp, [
    entry(1200, 10, 10, 1, 1190),   # row0 only
    entry(2500, 10, 20, 1, 2490),   # overlap 2000-3000
    entry(3500, 10, 30, 1, 3490),   # row1 only
], "c1")
tasks_p = write_tasks(tmp)
rc, o, out_rows = referee(tmp, rows_p, proxy_p, tasks_p, "c1")
check("C1a overlap call counted ONCE (row0 = 10+20, row1 = 30; "
      "pre-F127: row1 double-counts -> 50)",
      [r["prompt_tokens"] for r in out_rows], [30, 30])
check("C1b the overlap is NAMED", "overlapping rows" in o, True)
check("C1c warning PERSISTED in every row of the task",
      [bool(r.get("attribution_warnings")) and
       any("overlap" in w for w in r.get("attribution_warnings") or [])
       for r in out_rows], [True, True])

# --- C2: outside classes + untimestamped ---------------------------------
print()
print("=== C2. outside-all-rows classes: pre-span / gap / post-span / no-ts ===")
rows_p = write_rows(tmp, [(1000, 2000), (3000, 4000)])
no_ts = {"method": "POST", "path": "/v1/chat/completions", "status": 200,
         "latency_ms": 5, "usage": {"prompt_tokens": 11, "completion_tokens": 1}}
proxy_p = write_proxy(tmp, [
    entry(500, 10, 5, 1, 490),      # pre-span
    entry(2500, 10, 7, 1, 2490),    # gap
    entry(4500, 10, 9, 1, 4490),    # post-span
    no_ts,                          # untimestamped
], "c2")
rc, o, out_rows = referee(tmp, rows_p, proxy_p, tasks_p, "c2")
check("C2a pre-span calls NAMED", "pre-span 1" in o, True)
check("C2b gap calls NAMED", "gap 1" in o, True)
check("C2c post-span calls NAMED", "post-span 1" in o, True)
check("C2d untimestamped chat call NAMED and dropped",
      "untimestamped chat calls dropped: 1" in o, True)

# --- C3: non-chat POST inside a row ---------------------------------------
print()
print("=== C3. non-chat POSTs inside a row: excluded + NAMED ===")
rows_p = write_rows(tmp, [(1000, 2000)])
proxy_p = write_proxy(tmp, [
    entry(1200, 10, 10, 1, 1190),
    entry(1400, 10, 50, 1, 1390, path="/v1/embeddings"),
], "c3")
rc, o, out_rows = referee(tmp, rows_p, proxy_p, tasks_p, "c3")
check("C3a embeddings tokens NOT summed into the row "
      "(pre-F127: 60)",
      [r["prompt_tokens"] for r in out_rows], [10])
check("C3b non-chat POST exclusion NAMED", "non-chat POSTs excluded: 1" in o, True)

# --- C5: audit robustness (no ts_ms / no latency_ms) ----------------------
print()
print("=== C5. audit: no-ts entry must not crash; named tolerance ===")
rows_p = write_rows(tmp, [(1000, 2000), (2000, 3000), (3000, 4000)])
proxy_p = write_proxy(tmp, [
    entry(1100, 10, 91, 26, 1090, body=BODY),
    entry(2100, 10, 91, 26, 2090, body=BODY),
    entry(3100, 10, 91, 26, 3090, body=BODY),
    entry(1500, None, 5, 1, body=BODY),         # latency-less (no req_ts_ms)
    {"method": "POST", "path": "/v1/chat/completions", "status": 200,
     "latency_ms": 10, "req_body": BODY,
     "req_sha256": hashlib.sha256(BODY.encode()).hexdigest(),
     "usage": {"prompt_tokens": 7, "completion_tokens": 1}},  # no ts_ms
], "c5")
rc, o = run([sys.executable, HERE / "audit_replica_prompt_identity.py",
             rows_p, proxy_p])
check("C5a no traceback (pre-F127: KeyError 'ts_ms')",
      "Traceback" in o, False)
check("C5b a verdict is still produced", "RESULT:" in o, True)
check("C5c untimestamped entry NAMED", "untimestamped" in o, True)
check("C5d latency-less receipt NAMED", "latency-less" in o, True)

# --- C6: analyze ISO_BATCHES re-publish filter -----------------------------
print()
print("=== C6. analyze.py ISO_BATCHES: re-publish from a chosen batch set ===")
(copy := tmp / "analyze-copy").mkdir()
(copy / "out").mkdir()
shutil.copy2(HERE / "analyze.py", copy / "analyze.py")

def arow(batch, ok, ptok):
    return {"harness_name": "golem", "task_id": "T1", "category": "file_ops",
            "pass_f72": ok, "prompt_tokens": ptok, "completion_tokens": 1,
            "wall_clock_ms": 100, "context_overflow_count": 0,
            "peak_ram_mb": 10, "spawns": 0, "gate_holds": 0,
            "spawns_definition": "gated-executions", "files_created": [],
            "files_removed": [], "exit_codes": [0], "verdict_detail": "x",
            "safety_mechanism": None, "replica": 0, "run_batch": batch,
            "iso_reps": 2, "sample": False}

(copy / "out" / "iso_benchmark.jsonl").write_text("".join(
    json.dumps(r) + "\n" for r in [arow("BATCH-A", True, 100),
                                   arow("BATCH-A", False, 100),
                                   arow("BATCH-B", True, 200),
                                   arow("BATCH-B", True, 200)]))
rc, o = run([sys.executable, copy / "analyze.py"],
            env={"ISO_BATCHES": "BATCH-A"}, cwd=copy)
md = (copy / "out" / "ISO_RESULTS.md").read_text()
check("C6a only the chosen batch is listed as included",
      "Measurement batches included: BATCH-A " in md, True)
check("C6b pass rate reflects ONLY BATCH-A (1/2; pre-F127: 3/4)",
      "| **pass rate (all replicas)** | 1/2 | not-run | not-run |" in md, True)
check("C6c the filter is VISIBLE in the report",
      "Batch filter (ISO_BATCHES)" in md, True)

print()
print(f"=== {passed} passed, {failed} failed, {skipped} skipped ===")
if failed:
    print(f"(temp fixtures kept for inspection: {tmp})")
else:
    shutil.rmtree(tmp, ignore_errors=True)
sys.exit(1 if failed else 0)
