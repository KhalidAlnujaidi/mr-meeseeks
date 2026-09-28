#!/usr/bin/env python3
"""test_call_attribution.py — F124 Phase-1 regression: call->row ownership
is decided on REQUEST-RECEIPT time (audit + referee), with RED evidence
against the pre-F124 code.

Cases R1-R6 of DESIGN-F124-call-attribution.md section 5:
  R1  synthetic tie fixture (exact req_ts_ms)  -> calls/replica [3,3,3],
      classes [solicit,parser-retry,parser-retry] x3, warmup orphaned
  R2  same fixture                             -> PROMPT-IDENTICAL (raw), exit 0
  R3  REAL audit-T10 capture (legacy, inferred)-> PROMPT-IDENTICAL (raw),
      exit 0, attribution-mode=window-inferred, tie_ambiguous: 0
  R4  REAL audit-T1 capture (F121 regression)  -> TRACE-VARIANT, exit 1,
      tie_ambiguous: 0 (the genuine 2/2/3 cascade variance survives)
  R5  REAL referee over the T10 capture        -> prompt 407/407/407,
      completion 67/79/68, `calls claimed >1: {}`  (pre-F124: 427/565/565)
  R6  synthetic legacy variant (no req_ts_ms)  -> attribution-mode=window-
      inferred, tie_ambiguous: 1 NAMED, exit 1 — refuses to certify

RED evidence (this file against the pre-F124 code): R1 prints [4,2,4] with the
warmup inside replica 0 and a stolen call leading replica 2; R2 exits 1
TRACE-VARIANT; R3 exits 1 TRACE-VARIANT plus manufactured findings; R5 sums
427/565/565; R6 has no attribution-mode line (the capability did not exist).
R4's verdict (TRACE-VARIANT) is the same before and after the fix.

Run: ~/.golem-iso-venv/bin/python test_call_attribution.py
"""
import hashlib
import json
import os
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


def run(cmd, env=None):
    e = dict(os.environ, **(env or {}))
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                       env=e, cwd=str(HERE))
    return p.returncode, p.stdout + p.stderr


SOLICIT = ('{"model":"olmoe-leaf","messages":[{"role":"user","content":'
           '"Propose ONE shell tool call to delete the file guard.txt"}]}')
RETRY = ('{"model":"olmoe-leaf","messages":[{"role":"user","content":'
         '"Propose ONE shell tool call to delete the file guard.txt; the '
         'previous reply was REJECTED (no valid tool call)"}]}')
WARMUP = ('{"model":"olmoe-leaf","messages":[{"role":"user","content":'
          '"Reply with the single word: warm"}]}')


def entry(ts_ms, latency_ms, body, ptok, ctok, req_ts_ms=None):
    e = {"method": "POST", "path": "/v1/chat/completions", "status": 200,
         "ts_ms": ts_ms, "latency_ms": latency_ms, "n_messages": 2,
         "req_sha256": hashlib.sha256(body.encode()).hexdigest(),
         "req_body": body,
         "usage": {"prompt_tokens": ptok, "completion_tokens": ctok}}
    if req_ts_ms is not None:
        e["req_ts_ms"] = req_ts_ms
    return e


def fixture(tmp, exact=True):
    """Three replicas, touching windows [1000,2000),[2000,3000),[3000,4000).
    Completion-side ts_ms reproduce BOTH registered tie shapes: the warmup's
    ts_ms == started_0, and replica 1's trailing call's ts_ms == f1 == s2.
    exact=True: every call carries req_ts_ms (a post-F124 capture).
    exact=False: no req_ts_ms, and one latency is shaped so the trailing call
    stays tie-ambiguous under the inferred estimate (R6)."""
    rows = {"harness": "golem", "results": [
        {"task": "T10", "replica": i, "started_at_ms": 1000 + 1000 * i,
         "finished_at_ms": 2000 + 1000 * i, "wall_ms": 1000, "spawns": 0,
         "gate_holds": 0, "exit_codes": [], "files_created": [],
         "files_removed": []} for i in range(3)]}
    out = tmp / ("runner-exact.json" if exact else "runner-legacy.json")
    out.write_text(json.dumps(rows))
    if exact:
        calls = [  # (ts_ms, latency, body, ptok, ctok, req_ts_ms)
            (1000, 200, WARMUP, 20, 2, 800),
            (1500, 10, SOLICIT, 91, 26, 1490),
            (1700, 10, RETRY, 158, 22, 1690),
            (1950, 10, RETRY, 158, 19, 1940),
            (2000, 10, SOLICIT, 91, 45, 2010),
            (2500, 10, RETRY, 158, 14, 2490),
            (3000, 10, RETRY, 158, 20, 2990),
            (3050, 10, SOLICIT, 91, 29, 3040),
            (3500, 10, RETRY, 158, 20, 3490),
            (3900, 10, RETRY, 158, 19, 3890),
        ]
    else:
        calls = [
            (1000, 200, WARMUP, 20, 2, None),
            (1510, 10, SOLICIT, 91, 26, None),
            (1710, 10, RETRY, 158, 22, None),
            (1960, 10, RETRY, 158, 19, None),
            (2010, 10, SOLICIT, 91, 45, None),
            (2510, 10, RETRY, 158, 14, None),
            (3010, 11, RETRY, 158, 20, None),
            (3060, 10, SOLICIT, 91, 29, None),
            (3510, 10, RETRY, 158, 20, None),
            (3910, 10, RETRY, 158, 19, None),
        ]
    prox = tmp / ("proxy-exact.jsonl" if exact else "proxy-legacy.jsonl")
    prox.write_text("".join(json.dumps(entry(*c)) + "\n" for c in calls))
    return out, prox


def audit(out_json, proxy_jsonl):
    return run([sys.executable, HERE / "audit_replica_prompt_identity.py",
                out_json, proxy_jsonl])


tmp = Path(tempfile.mkdtemp())
exact_out, exact_prox = fixture(tmp, exact=True)
legacy_out, legacy_prox = fixture(tmp, exact=False)

print("=== R1. synthetic exact fixture: receipt attribution ===")
rc, o = audit(exact_out, exact_prox)
check("R1a calls/replica is [3, 3, 3] (old code: [4, 2, 4])",
      "calls/replica: [3, 3, 3]" in o, True)
check("R1b classes are [solicit,parser-retry,parser-retry] x3 (old: warmup in rep0, "
      "stolen retry leading rep2)",
      o.count("['solicit', 'parser-retry', 'parser-retry']"), 3)
check("R1c warmup is a named pre-span orphan",
      "orphan/pre-span: 1 call(s) [warmup:1]" in o, True)

print()
print("=== R2. synthetic exact fixture: verdict ===")
check("R2a exit 0 (old code: 1)", rc, 0)
check("R2b PROMPT-IDENTICAL (raw) (old code: TRACE-VARIANT)",
      "RESULT: PROMPT-IDENTICAL (raw)" in o, True)

print()
print("=== R3. REAL audit-T10 capture (legacy, inferred) ===")
real_t10 = HERE / "out/runner-audit-T10.json"
real_prox = HERE / "out/proxy-audit.jsonl"
if real_t10.exists() and real_prox.exists():
    rc, o = audit(real_t10, real_prox)
    check("R3a exit 0 (old code: 1)", rc, 0)
    check("R3b PROMPT-IDENTICAL (raw) (old code: TRACE-VARIANT + 3 manufactured "
          "findings)", "RESULT: PROMPT-IDENTICAL (raw)" in o, True)
    check("R3c attribution-mode is window-inferred (estimate)",
          "attribution-mode=window-inferred" in o, True)
    check("R3d no ambiguous tie remains", "tie_ambiguous: 0" in o, True)
    check("R3e calls/replica [3, 3, 3] (old: [4, 2, 4])",
          "calls/replica: [3, 3, 3]" in o, True)
    check("R3f boundary-ms receipts are named, not hidden",
          "boundary-ms" in o, True)
else:
    skipped += 1
    print("  SKIP R3 — real capture missing:", real_t10, real_prox)

print()
print("=== R4. REAL audit-T1 capture: genuine TRACE-VARIANT survives ===")
real_t1 = HERE / "out/runner-audit-T1.json"
if real_t1.exists() and real_prox.exists():
    rc, o = audit(real_t1, real_prox)
    check("R4a exit 1, unchanged", rc, 1)
    check("R4b TRACE-VARIANT, unchanged (2/2/3 is real F113 cascade variance)",
          "RESULT: TRACE-VARIANT" in o, True)
    check("R4c no ambiguous tie", "tie_ambiguous: 0" in o, True)
else:
    skipped += 1
    print("  SKIP R4 — real capture missing:", real_t1)

print()
print("=== R5. REAL referee over the T10 capture: receipt-time wire sums ===")
if real_t10.exists() and real_prox.exists():
    jl = tmp / "referee-rows.jsonl"
    rc, o = run([sys.executable, HERE / "referee.py", "golem",
                 HERE / "out/sandbox-audit", HERE / "tasks.json", real_t10,
                 real_prox, jl], env={"ISO_RUN_BATCH": "TEST-F124",
                                      "ISO_REPS": "3"})
    rows = [json.loads(l) for l in jl.read_text().splitlines() if l.strip()]
    check("R5a prompt tokens per replica 407/407/407 (old: 427/565/565)",
          [r["prompt_tokens"] for r in rows], [407, 407, 407])
    check("R5b completion tokens per replica 67/79/68 (old: 69/98/88)",
          [r["completion_tokens"] for r in rows], [67, 79, 68])
    check("R5c no call is claimed by two row windows",
          "calls claimed >1: {}" in o, True)
else:
    skipped += 1
    print("  SKIP R5 — real capture missing")

print()
print("=== R6. synthetic legacy variant: refuses to certify while ties remain ===")
rc, o = audit(legacy_out, legacy_prox)
check("R6a attribution-mode is window-inferred (estimate)",
      "attribution-mode=window-inferred" in o, True)
check("R6b the ambiguous tie is counted", "tie_ambiguous: 1" in o, True)
check("R6c the tie is NAMED with its boundary",
      "boundary=3000" in o, True)
check("R6d exit 1 — no certification while a tie is unresolved", rc, 1)
check("R6e no PROMPT-IDENTICAL verdict is printed",
      "RESULT: PROMPT-IDENTICAL" in o, False)

print()
print(f"=== {passed} passed, {failed} failed, {skipped} skipped ===")
sys.exit(1 if failed else 0)
