#!/usr/bin/env python3
"""test_call_attribution.py — F124 Phase-1 regression: call->row ownership
is decided on REQUEST-RECEIPT time (audit + referee), with RED evidence
against the pre-F124 code.

Cases R1-R6 of DESIGN-F124-call-attribution.md section 5:
  R1  synthetic tie fixture (exact req_ts_ms)  -> calls/replica [3,3,3],
      classes [solicit,parser-retry,parser-retry] x3, warmup orphaned
  R2  same fixture                             -> PROMPT-IDENTICAL (raw), exit 0
  R3  REAL audit-T10 capture (legacy, inferred)-> manufactured findings
      DELETED, traces match [solicit,parser-retry,parser-retry]x3, but the one
      shared-boundary receipt stays NAMED: TRACE-NOT-CERTIFIABLE, exit 1 via
      tie-refusal (NOT via TRACE-VARIANT); attribution-mode=window-inferred
  R4  REAL audit-T1 capture (F121 regression)  -> TRACE-VARIANT, exit 1,
      tie_ambiguous: 0 (the genuine 2/2/3 cascade variance survives)
  R5  REAL referee over the T10 capture        -> prompt 407/407/407,
      completion 67/79/68, `calls claimed >1: {}`  (pre-F124: 427/565/565)
  R6  synthetic legacy variant (no req_ts_ms)  -> attribution-mode=window-
      inferred, tie_ambiguous: 2 NAMED, exit 1 — refuses to certify
  R6f synthetic outer-edge variant (F126)      -> a receipt landing exactly
      ON started_0 is row-vs-orphan ambiguous (est = f+1 is possible under
      the two-sided ±1 ms bound): NAMED as boundary-ms, refuses exit 0
      (pre-F126: certified PROMPT-IDENTICAL — a false certification)

RED evidence (this file against the pre-F124 code): R1 prints [4,2,4] with the
warmup inside replica 0 and a stolen call leading replica 2; R2 exits 1
TRACE-VARIANT; R3 exits 1 with ZERO manufactured findings (all 3 deleted) but
refuses certification via the single NAMED shared-boundary tie; R5 sums
427/565/565; R6 has no attribution-mode line (the capability did not exist).
R4's verdict (TRACE-VARIANT) is the same before and after the fix.
R6f (added with the F126 hardening) fails against the pre-F126 code: the
outer-edge fixture certified `PROMPT-IDENTICAL (raw)` exit 0 while the
boundary receipt's row-vs-orphan ownership is unprovable from stamps.

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
    # RECORDED EXPECTATION CHANGE ON REAL DATA: the 3 manufactured findings
    # (call-count + 2 alignment-offset STRUCTURALs) are DELETED -- a fabricated
    # finding removed, NOT a real finding softened. The traces now match
    # ([solicit,parser-retry,parser-retry]x3, byte-identical per index), but the
    # one shared-boundary receipt stays NAMED and refuses exit 0 per contract.
    check("R3a exit 1 via tie-refusal, NOT via manufactured findings (old: exit 1 "
          "via TRACE-VARIANT + 3 findings)", rc, 1)
    check("R3b NO manufactured findings survive",
          ("VARIES(kind=trace" not in o and "STRUCTURAL(" not in o), True)
    check("R3b2 traces match but verdict refuses certification",
          ("RESULT: TRACE-NOT-CERTIFIABLE" in o
           and "prompt INPUTS are byte-identical" in o), True)
    check("R3c attribution-mode is window-inferred (estimate)",
          "attribution-mode=window-inferred" in o, True)
    check("R3d one shared-boundary tie remains, NAMED, and refuses exit 0",
          ("tie_ambiguous: 1" in o
           and "TIE(kind=attribution," in o
           and "RESULT: TRACE-NOT-CERTIFIABLE" in o), True)
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
check("R6b both ambiguous ties are counted", "tie_ambiguous: 2" in o, True)
check("R6c the tie is NAMED with its boundary",
      "candidates=[3000]" in o, True)
check("R6d exit 1 — no certification while a tie is unresolved", rc, 1)
check("R6e no PROMPT-IDENTICAL verdict is printed",
      "RESULT: PROMPT-IDENTICAL" in o, False)

print()
print("=== R6f. outer-edge receipt (row-vs-orphan ambiguity) — F126 ===")
# The estimate error is TWO-SIDED (±1 ms): est = f+1 is possible for a true
# receipt floor f. A receipt whose estimate lands exactly ON an outer span
# edge (started_0) could therefore truly be a PRE-SPAN ORPHAN; naming it
# boundary-ms while certifying the trace is a false certification (F126,
# found by the second verifier's outer-edge fixture). It must refuse.
out_edge = tmp / "outer-edge"
out_edge.mkdir()
rows_oe = {"harness": "golem", "results": [
    {"task": "T10", "replica": i, "started_at_ms": 1000 + 1000 * i,
     "finished_at_ms": 2000 + 1000 * i, "wall_ms": 1000, "spawns": 0,
     "gate_holds": 0, "exit_codes": [], "files_created": [],
     "files_removed": []} for i in range(3)]}
(out_edge / "runner.json").write_text(json.dumps(rows_oe))
calls_oe = [
    (1001, 1, WARMUP, 20, 2, None),      # est 1000 == started_0 (outer edge)
    (1500, 10, SOLICIT, 91, 26, None),
    (1700, 10, RETRY, 158, 22, None),
    (2010, 1, WARMUP, 20, 2, None),      # est 2009 -> replica 1
    (2500, 10, SOLICIT, 91, 45, None),
    (2700, 10, RETRY, 158, 14, None),
    (3010, 1, WARMUP, 20, 2, None),      # est 3009 -> replica 2
    (3500, 10, SOLICIT, 91, 29, None),
    (3700, 10, RETRY, 158, 20, None),
]
(out_edge / "proxy.jsonl").write_text(
    "".join(json.dumps(entry(*c)) + "\n" for c in calls_oe))
rc, o = audit(out_edge / "runner.json", out_edge / "proxy.jsonl")
check("R6f the outer-edge receipt is NAMED as boundary-ms",
      ("boundary-ms receipts" in o and "[warmup@1000]" in o), True)
check("R6f2 row-vs-orphan ambiguity refuses certification "
      "(pre-F126: exit 0 + PROMPT-IDENTICAL — false certification)",
      (rc, "RESULT: TRACE-NOT-CERTIFIABLE" in o,
       "RESULT: PROMPT-IDENTICAL" in o), (1, True, False))

print()
print(f"=== {passed} passed, {failed} failed, {skipped} skipped ===")
sys.exit(1 if failed else 0)
