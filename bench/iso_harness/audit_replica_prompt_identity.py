#!/usr/bin/env python3
"""audit_replica_prompt_identity.py — Prompt-identity audit (Phase 1, plan
2026-09-26_211500, Task 1.2).

Question: were the bench's replicas actually "the same prompt" (README F113/
F115 wording), or do prompt INPUTS differ across replicas — making k/n flips
conflate prompt-variance with rollout-variance?

Inputs: a runner out.json (per-replica windows) and the proxy JSONL produced
by a single instrumented rerun (ISO_PROXY_CAPTURE=1 so req_body is present).

What is measured (two SEPARATE properties — the verdict names which one
moved; conflating them is exactly the defect this script must not repeat):
  - PROMPT-INPUT identity: the base *solicitation* body (`solicit` class —
    the task-bearing user message) is byte-identical across replicas.
  - TRACE identity: replicas made the same NUMBER and SEQUENCE of
    chat-completion calls.

Property asserted (PASS only when ALL of these hold, exit 0):
  1. every replica made the SAME number of chat-completion calls (TRACE),
  2. call-by-call request bodies are byte-identical RAW, OR identical after
     the STRICT allowlist normalization below.
Anything else exits 1 with VARIES(reason=...) lines — per the plan that
counts as a FINDING, not a script failure.

Verdict taxonomy on exit 1 (the distinction lives in the TEXT and in the
`kind=` reason codes, NEVER in the exit code — both kinds still exit 1):
  - PROMPT-VARIANT: the base solicit body itself DIFFERS across replicas.
    That is genuine prompt-INPUT variance and it undermines the README
    "same prompt" claim (F113/F115).
  - TRACE-VARIANT: the base solicit body is BYTE-IDENTICAL across every
    replica, yet call count/sequence differs. The variance is then in the
    CALL TRACE only — warmup placement (F112) and the parser-retry cascade
    (F113) are control flow reacting to model output, NOT prompt inputs.
    k/n therefore measures MODEL rollout variance conditional on IDENTICAL
    inputs: exactly what F115 claims, which this audit does NOT undermine.
  - PROMPT-VARIANT (trace also varies): both moved; input variance is the
    claim-breaking part, so it is reported as input variance.
Asymmetry worth keeping: identity of the base solicit body is the
claim-bearing measurement, so a differing call count can never excuse a
missing or divergent solicit body — that stays input variance / UNEXPLAINED.
Normalization allowlist (conservative; every rule must NAMED-reappear in the
report; anything unmatched is reported as UNEXPLAINED — never silently ok):
  - ephemeral workspace id:   ws_<36 hex/dash>        (F88 isolation uuid)
  - temp root paths:          /var/folders/...        (GOLEM_WORKSPACE_ROOT)
  - ISO-8601 timestamps:      2026-09-27T12:00:00     (if a prompt embeds one)
Nothing else is normalized: numbers, wording, tool names stay verbatim.

F124 Phase 1 (receipt-time attribution) — call->row ownership is decided on
REQUEST-RECEIPT time, never on the completion-side `ts_ms`:
  - `req_ts_ms` (proxy.py stamps it at receipt) when present — exact;
  - else `ts_ms - latency_ms` (the same receipt, reconstructed) — INFERRED,
    with a one-sided error: floor(receipt) in {est, est+1} (ms flooring of
    both stamps plus latency rounding; the proxy's two time() reads are
    sub-ms apart). Consequences, used below:
      * est == boundary-1 is the ONLY inferred value whose floor set
        straddles a row boundary -> counted in `tie_ambiguous`, NAMED, and a
        trace verdict refuses exit 0 while any remain (input verdicts are
        unaffected by tie resolution).
      * est == boundary is floor-deterministic (the later row owns it) and
        is NAMED as a boundary-ms receipt: the sub-ms ordering question at a
        boundary millisecond is unobservable from ms stamps and stays open
        until Phase-2 call tags; it is printed, not silently certified.
Calls outside every row window (pre-roll warmup) are listed as orphans and
excluded from replica traces — never silently dropped, never counted.

Usage:
  python3 audit_replica_prompt_identity.py OUT_JSON PROXY_JSONL
"""
import difflib
import hashlib
import json
import re
import sys

NORM_RULES = [
    (re.compile(r"ws_[0-9a-fA-F-]{36}"), "ws_<UUID>"),
    (re.compile(r"/var/folders/[A-Za-z0-9_./-]+"), "<TMP>"),
    (re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z?"), "<ISO>"),
]


def normalize(body: str) -> str:
    for pat, repl in NORM_RULES:
        body = pat.sub(repl, body)
    return body


def classify(body: str) -> str:
    """Coarse role of a call inside the golem turn shape. Kept to exact
    registered markers (F57 retry text, F112 warmup) — anything unmarked
    returns 'other' and is NEVER used to excuse a diff."""
    if "previous reply was REJECTED" in body:
        return "parser-retry"
    if body.startswith('{"model"') and '"content":"Reply with the single word' in body:
        return "warmup"
    try:
        c = json.loads(body).get("messages", [{}])[0].get("content", "")
    except Exception:
        c = ""
    if c.startswith("Reply with the single word"):
        return "warmup"
    if "Propose ONE shell tool call" in c:
        return "solicit"
    return "other"


def sha(s: str) -> str:
    return hashlib.sha256(s.encode()).hexdigest()[:16]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    out_path, proxy_path = sys.argv[1], sys.argv[2]

    runner = json.load(open(out_path))
    rows = runner.get("results") or runner.get("rows") or []
    if not rows:
        print(f"FAIL: no per-replica entries found in {out_path} "
              f"(top keys: {list(runner.keys())})")
        return 2

    entries = [json.loads(l) for l in open(proxy_path)]
    chat = [e for e in entries
            if e.get("method") == "POST"
            and "/chat/completions" in str(e.get("path", ""))]
    missing = [i for i, e in enumerate(chat) if "req_body" not in e]
    if missing:
        print(f"FAIL: {len(missing)} chat entries lack req_body — "
              "rerun with ISO_PROXY_CAPTURE=1 (bodies are never retroactive)")
        return 2

    # F124 Phase 1: attribute every chat call to a replica row by REQUEST
    # RECEIPT time. The old rule used ts_ms (completion side, stamped after
    # the response was written back): adjacent rows touch exactly
    # (finish_i == start_i+1) and completion stamps land ON boundaries, so
    # the half-open test stole replica 1's trailing call into replica 2 and
    # the pre-roll warmup into replica 0 (observed on the real T10 capture:
    # [4,2,4] against a truth of [3,3,3]). On the receipt side each request
    # is received while exactly one row scope is live.
    def attributed(e):
        if e.get("req_ts_ms") is not None:
            return e["req_ts_ms"], "exact"
        return e["ts_ms"] - (e.get("latency_ms") or 0), "inferred"

    att = [(e, *attributed(e)) for e in chat]
    modes = {m for _, _, m in att}
    if modes == {"exact"}:
        mode_label = "exact (req_ts_ms)"
    elif modes == {"inferred"}:
        mode_label = "window-inferred (estimate: floor(receipt) in {est, est+1})"
    else:
        mode_label = "mixed (exact where req_ts_ms was logged)"
    print(f"attribution-mode={mode_label}")

    windows = []
    for r in sorted(rows, key=lambda r: r.get("replica", 0)):
        rep = r.get("replica", None)
        if r.get("started_at_ms") is None or r.get("finished_at_ms") is None:
            print(f"FAIL: replica {rep} lacks a time window "
                  "(started_at_ms/finished_at_ms) — cannot attribute calls")
            return 2
        win = [e for e, a, m in att
               if r["started_at_ms"] <= a < r["finished_at_ms"]]
        windows.append((rep, win))

    # Tie accounting. Boundaries = every row start and finish (touching rows
    # share one ms value). Inferred est == b-1: floor set {b-1, b} straddles
    # the boundary -> AMBIGUOUS; blocks trace certification. Exact req == b,
    # or inferred est == b: the floor rule assigns to the later row
    # deterministically, but the receipt sits in the boundary millisecond —
    # NAMED as a boundary-ms receipt (sub-ms ordering is unobservable from ms
    # stamps; Phase-2 call tags close this).
    boundaries = sorted({r["started_at_ms"] for r in rows}
                        | {r["finished_at_ms"] for r in rows})
    sorted_rows = sorted(rows, key=lambda r: r.get("replica", 0))
    ties, bms = [], []
    for e, a, m in att:
        # A receipt exactly ON a shared boundary (finished_N == started_N+1)
        # is the tie the old window stole or dropped: candidates are the
        # adjacencies on both sides. The half-open floor rule still assigns
        # every call exactly one window (checked below), but the assignment
        # is a truncation coin-flip — NAMED, and a trace verdict refuses
        # exit 0 while any remain.
        cands = sorted({rr.get("replica") for rr in sorted_rows
                        if rr["started_at_ms"] == a or rr["finished_at_ms"] == a})
        if len(cands) > 1 or (m == "inferred"
                              and any(a == b - 1 for b in boundaries)):
            b = a if len(cands) > 1 else next(bb for bb in boundaries
                                              if a == bb - 1)
            ties.append((e, a, sorted(cands) if cands else [b]))
            print(f"TIE(kind=attribution, ts={a}, "
                  f"candidates={sorted(cands) if cands else [b]})")
        elif any(a == b for b in boundaries):
            bms.append(f"{classify(e['req_body'])}@{a}")
    print(f"tie_ambiguous: {len(ties)}"
          + (f" — {', '.join(t[0]['req_body'] and classify(t[0]['req_body']) + '@' + str(t[1]) for t in ties)}" if ties else ""))
    if bms:
        print(f"boundary-ms receipts (floor rule assigns to the later row; "
              f"sub-ms ordering unobservable from ms stamps until Phase-2 "
              f"tags): {len(bms)} call(s) [{', '.join(bms)}]")

    # Attribution must be a PARTITION, not a hope: no call may be claimed by
    # two windows (identity, not dict equality — two byte-equal entries were
    # indistinguishable under the old `e in w` test), and every call inside
    # the overall span must be claimed exactly once. Calls outside every row
    # are ORPHANS (pre-roll warmup etc.) — listed, excluded, never counted.
    claimed_ids = {id(e) for _, w in windows for e in w}
    dup = [a for e, a, m in att
           if sum(1 for _, w in windows if any(x is e for x in w)) > 1]
    span_lo = min(r["started_at_ms"] for r in rows)
    span_hi = max(r["finished_at_ms"] for r in rows)
    in_span = [a for _, a, _ in att if span_lo <= a < span_hi]
    if dup or len(claimed_ids) != len(in_span):
        print(f"FAIL: window attribution is not a partition — "
              f"{len(dup)} call(s) claimed twice, {len(claimed_ids)} claimed "
              f"vs {len(in_span)} inside the overall span; calls/replica "
              "would be fiction")
        return 2
    orphans = [(e, a) for e, a, m in att if id(e) not in claimed_ids]
    for label, sel in (("pre-span", [o for o in orphans if o[1] < span_lo]),
                       ("post-span", [o for o in orphans if o[1] >= span_hi])):
        if sel:
            by_class = {}
            for e, a in sel:
                c = classify(e["req_body"])
                by_class[c] = by_class.get(c, 0) + 1
            named = "[" + ", ".join(f"{c}:{n}" for c, n in sorted(by_class.items())) + "]"
            print(f"orphan/{label}: {len(sel)} call(s) {named} — excluded "
                  "from replica traces (counted in no row)")

    print(f"replicas: {[w[0] for w in windows]}  "
          f"calls/replica: {[len(w[1]) for w in windows]}")

    findings = []
    counts = {len(w[1]) for w in windows}
    # A vacuous comparison is NOT a pass: if no window attributed any
    # chat call, we have no measurement at all (attribution bug or empty
    # run) — refuse to judge rather than let `all([])` read as identical.
    if not any(counts):
        print(f"FAIL: zero chat calls attributed to any replica"
              f" ({len(chat)} chat entries exist in the log) — "
              "windows vs proxy timestamps do not overlap; cannot judge")
        return 2
    if len(counts) != 1:
        findings.append(f"VARIES(kind=trace, reason=call-count across "
                        f"replicas: {[(w[0], len(w[1])) for w in windows]})")

    # compare call-by-call across the shortest replica span
    n_calls = min(counts) if counts else 0
    sequence_sha = []
    for _, win in windows:
        sequence_sha.append([sha(e["req_body"]) for e in win])

    raw_identical = all(seq == sequence_sha[0] for seq in sequence_sha) \
        if sequence_sha else False
    print("raw call-by-call sha match:", raw_identical,
          "seqs:", sequence_sha)

    norm_seqs = [[sha(normalize(e["req_body"])) for e in win]
                 for _, win in windows]
    norm_identical = all(seq == norm_seqs[0] for seq in norm_seqs)
    print("normalized call-by-call sha match:", norm_identical)

    if not norm_identical:
        for k in range(n_calls):
            bodies = [normalize(windows[i][1][k]["req_body"])
                      for i in range(len(windows))
                      if k < len(windows[i][1])]
            if len({b for b in bodies}) > 1:
                a = bodies[0]
                b = next(x for x in bodies[1:] if x != a)
                diff = list(difflib.unified_diff(
                    json.dumps(json.loads(a), indent=1, sort_keys=True)
                    .splitlines(),
                    json.dumps(json.loads(b), indent=1, sort_keys=True)
                    .splitlines(),
                    lineterm="", n=1))[2:14]
                ca = classify(windows[0][1][k]["req_body"])
                cb = classify(b)
                if ca != cb:
                    # Alignment offset: the two replicas are at different
                    # points of their own call sequence (warmup placement,
                    # retry cascade) — a TRACE property, not a premise change.
                    findings.append(
                        f"VARIES(kind=trace, call_index={k}, reason="
                        f"STRUCTURAL({ca}-vs-{cb} alignment-offset))")
                elif ca in ("parser-retry", "warmup"):
                    findings.append(
                        f"VARIES(kind=trace, call_index={k}, reason="
                        f"STRUCTURAL({ca}-count varies — downstream of "
                        "model output, see engine probe))")
                else:
                    # Same class at the same index, yet the body differs:
                    # this is the case that really is an INPUT difference.
                    known = []
                    for pat, repl in NORM_RULES:
                        if pat.search(a) and repl in a:
                            known.append(repl)
                    reason = ("allowlisted:" + ",".join(known)) if known \
                        else "UNEXPLAINED"
                    findings.append(
                        f"VARIES(kind=input, call_index={k}, "
                        f"class={ca}, reason={reason})")
                for line in diff:
                    print("   " + line)

    # per-replica message-count profile (round-count growth detector)
    profile = [(w[0], len(w[1]), [e.get("n_messages") for e in w[1]])
               for w in windows]
    print("per-replica (rep, calls, n_messages):", profile)
    classes = [(w[0], [classify(e["req_body"]) for e in w[1]])
               for w in windows]
    print("per-replica call classes:", classes)
    # Derive the base-solicit identity from the BODY actually compared
    # (never from a stored req_sha256 field): the verdict must be a function
    # of the same bytes the call-by-call comparison uses, or the two lines
    # can contradict each other.
    first_sha = {}
    for rep, win in windows:
        first_sha[rep] = next(
            (sha(e["req_body"]) for e in win
             if classify(e["req_body"]) == "solicit"), None)
    tok = first_sha
    short = {r: (None if v is None else v[:16]) for r, v in tok.items()}
    print("base solicit sha per replica:", [tok[r] for r, _ in windows],
          "-> BYTE-IDENTICAL" if None not in tok.values()
          and len(set(tok.values())) == 1 else "-> VARIES")

    # ---- F124 Phase 1: trace-certification gate ---------------------------
    # Input (base-solicit) verdicts do not depend on tie resolution; the
    # TRACE verdict does: while any call/row tie is ambiguous, a trace
    # identity — even a matching one — cannot be certified from stamps
    # alone. Refuse loudly (exit 1) rather than print a verdict the evidence
    # cannot carry.
    input_varies = (None in tok.values() or len(set(tok.values())) != 1)
    if ties:
        if input_varies:
            print("RESULT: PROMPT-VARIANT — the base solicitation body is "
                  "NOT byte-identical across replicas (input variance; it "
                  "does not depend on tie resolution). NOTE: the trace "
                  f"check is NOT CERTIFIABLE — {len(ties)} ambiguous tie(s) "
                  "named above.")
        else:
            print(f"RESULT: TRACE-NOT-CERTIFIABLE — {len(ties)} call/row "
                  "tie(s) remain ambiguous under receipt-time attribution "
                  "(named above); prompt INPUTS are byte-identical, but the "
                  "call traces cannot be certified from stamps alone.")
        for f in findings:
            print(" ", f)
        return 1

    if raw_identical or norm_identical:
        verdict = ("PROMPT-IDENTICAL (raw)" if raw_identical else
                   "PROMPT-IDENTICAL (after allowlist normalization)")
        print(f"RESULT: {verdict} across all replicas"
              + ("" if not findings else f" with notes: {findings}"))
        return 0

    # ---- exit-1 verdicts: separate INPUT variance from TRACE variance ----
    input_findings = [f for f in findings if "kind=input" in f]
    mech = sorted({c for _, win in windows
                   for c in (classify(e["req_body"]) for e in win)
                   if c in ("warmup", "parser-retry")})

    if input_varies:
        print("RESULT: PROMPT-VARIANT — the base solicitation body "
              "(the task-bearing user message) is NOT byte-identical across "
              "replicas; README 'same prompt' (F113/F115 wording) does not "
              "hold at byte level.")
        print("  base solicit sha per replica:",
              [tok[r] for r, _ in windows])
        print("  => this IS prompt-INPUT variance: k/n here would conflate "
              "prompt variance with rollout variance (the inability F115's "
              "wording must be read with).")
        for f in findings:
            print(" ", f)
        return 1
    else:
        print("RESULT: TRACE-VARIANT — prompt INPUTS are BYTE-IDENTICAL "
              "across all replicas (base solicit sha "
              f"{short[windows[0][0]]}); only the CALL TRACE differs "
              f"(calls/replica {[len(w[1]) for w in windows]}).")
        print("  (i) prompt INPUTS byte-identical — same task-bearing "
              "solicitation body in every replica, sha "
              f"{short[windows[0][0]]}; the 'same prompt' claim holds.")
        print("  (ii) call traces differ in NUMBER and SEQUENCE — "
              "per-replica call classes "
              f"{[(w[0], [classify(e['req_body']) for e in w[1]]) for w in windows]}"
              + (f", mechanism: {', '.join(mech)}" if mech else ""))

        print("  (iii) therefore this call-count variance is F112/F113 "
              "CONTROL FLOW (warmup placement / parser-retry cascade "
              "reacting to model output), NOT prompt-input variance; k/n "
              "measures MODEL rollout variance conditional on IDENTICAL "
              "inputs — exactly what F115 claims, not undermined.")
        if input_findings:
            print("  NOTE: input-tagged findings also present alongside the "
                  f"trace variance: {input_findings}")

    for f in findings:
        print(" ", f)
    return 1


if __name__ == "__main__":
    sys.exit(main())