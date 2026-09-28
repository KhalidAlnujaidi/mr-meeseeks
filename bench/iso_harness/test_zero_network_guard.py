#!/usr/bin/env python3
"""test_zero_network_guard.py — regression tests for F123 (zero-network law).

F93 states the harness must not be able to reach an outside model. Before this
fix that law was implemented as a per-invocation `env -u ...` prefix on ONE of
the three arms (golem only); smolagents and langgraph ran with the operator's
provider keys still exported, and nothing refused. These tests pin the fix: the
law is an enforced preflight gate plus a single scrub path every arm uses.

Khalid's doctrine: a regression test must FAIL on the old code. Check Z1 is the
red one — against the pre-fix script a key-set environment proceeded straight
past preflight, so the refusal check read False.

Run:  python3 test_zero_network_guard.py
"""
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "run_iso_bench.sh"
KEYS = ["OPENROUTER_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY", "TYPESAFE_API_KEY"]

passed = failed = 0


def check(name, got, want):
    global passed, failed
    if got == want:
        passed += 1
        print(f"  PASS {name}")
    else:
        failed += 1
        print(f"  FAIL {name}\n       got : {got!r}\n       want: {want!r}")


def clean_env(**extra):
    """Environment with every provider key removed, then `extra` applied."""
    env = {k: v for k, v in os.environ.items() if k not in KEYS}
    env.update(extra)
    env.setdefault("PATH", os.environ.get("PATH", ""))
    return env


def run(env, ids="T1"):
    """Invoke the script and return (rc, combined output).

    SAFETY: the clean path must never execute a real cell. We point the engine
    port at a dead port (9 = discard) so a clean environment stops at the
    engine-not-ready check (exit 4), which still proves preflight PASSED. The
    F123 refusal path fires BEFORE the engine probe, so it needs no engine.
    """
    r = subprocess.run(["bash", str(SCRIPT), "golem", ids],
                       env=env, capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout + r.stderr


print("=== Z. F123: the zero-network law is enforced, not a convention ===")

# Z1 — THE RED CHECK. A key-set environment must be REFUSED (exit 3).
rc, out = run(clean_env(OPENROUTER_API_KEY="sk-fake"))
check("Z1 key-set environment is REFUSED (exit 3), not run", rc == 3, True)
check("Z2 refusal names the offending variable", "OPENROUTER_API_KEY" in out, True)

# Z3 — every provider key triggers the gate, not just the one we tested.
for k in KEYS:
    rc_k, _ = run(clean_env(**{k: "x"}))
    check(f"Z3 gate fires for {k}", rc_k == 3, True)

# Z4 — multiple keys are all named.
rc, out = run(clean_env(OPENAI_API_KEY="a", ANTHROPIC_API_KEY="b"))
check("Z4 several keys are all named",
      ("OPENAI_API_KEY" in out and "ANTHROPIC_API_KEY" in out), True)

# Z5 — a clean environment must PASS preflight (dead engine => exit 4, not 3).
rc, out = run(clean_env(ENGINE_PORT="9"))
check("Z5 clean environment passes the gate (no REFUSING)",
      "REFUSING TO RUN" in out, False)
check("Z5b clean environment reports preflight OK",
      "zero-network law OK" in out, True)

# Z6 — structural: every arm invocation goes through the single scrub path.
# Pre-fix the smolagents/langgraph branches were invoked bare; this asserts the
# property that makes a fourth arm unable to repeat the mistake.
src = SCRIPT.read_text()
arm_calls = list(re.findall(r"ISO_WARMUP=\$first (run_scrubbed)?", src))
check("Z6 three arm invocations present", len(arm_calls), 3)
check("Z6b all arm invocations use run_scrubbed(), none bare",
      all(a.strip() == "run_scrubbed" for a in arm_calls), True)
check("Z6c no bare arm invocation remains",
      any(a.strip() == "" for a in arm_calls), False)

# Z7 — the ad-hoc per-branch prefix is gone; the scrub lives in exactly one
# helper definition, so there is one place to audit.
check("Z7 run_scrubbed helper is defined once",
      len(re.findall(r"^run_scrubbed\(\)", src, re.M)), 1)

# Z8 — BEHAVIOURAL: run_scrubbed must actually REMOVE the keys from the child
# environment. The first implementation used `${NETWORK_KEYS[@]/#/-u }`, which
# yields one argument per key ("-u OPENROUTER_API_KEY") that env rejects as
# malformed and which strips NOTHING — the child still saw the key. This check
# executes the real helper out of the real script, so that bug cannot return.
probe = r'''
set -euo pipefail
NETWORK_KEYS=(OPENROUTER_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY TYPESAFE_API_KEY)
''' + re.search(r"^run_scrubbed\(\) \{.*?\n\}", src, re.M | re.S).group(0) + r'''
export OPENROUTER_API_KEY=leaked OPENAI_API_KEY=leaked2
run_scrubbed bash -c 'echo "OPEN:${OPENROUTER_API_KEY:-UNSET}"; echo "OAI:${OPENAI_API_KEY:-UNSET}"'
'''
r = subprocess.run(["bash", "-c", probe], capture_output=True, text=True, timeout=60)
check("Z8 run_scrubbed strips keys from the child (behavioural)",
      "OPEN:UNSET" in r.stdout and "OAI:UNSET" in r.stdout, True)
check("Z8b no key survives in the child env",
      "leaked" not in r.stdout, True)

print()
print(f"=== {passed} passed, {failed} failed ===")
sys.exit(1 if failed else 0)
