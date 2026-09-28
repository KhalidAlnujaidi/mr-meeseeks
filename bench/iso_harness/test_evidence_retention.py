#!/usr/bin/env python3
"""test_evidence_retention.py — F125 regression: a scored batch's raw evidence
(arm proxy log, per-replica runner files, stdout logs) is BATCH-SUFFIXED and
never truncated; a colliding batch name is REFUSED, not overwritten.

Cases:
  E1  collision refusal — run with ISO_RUN_BATCH=X while evidence for X
      exists -> exit nonzero, "REFUSING" named, and BOTH the existing batch
      file and a legacy-named file are left byte-intact.
      (pre-F125: the script does `: > out/proxy-<arm>.jsonl` in place and runs
      on — the legacy sentinel is destroyed; RED evidence.)
  E2  per-replica retention — smolagents arm, ISO_REPS=2, one task -> TWO
      runner files (…-rep0-<batch>.json and …-rep1-<batch>.json) plus a
      batch-suffixed proxy log survive the run, and no legacy-named file is
      created.
      (pre-F125: ONE runner file — last replica wins — and an un-suffixed
      log; RED evidence.)
  E3  golem naming — one runner file per task, batch-suffixed (the shell does
      not loop golem replicas; the C++ runner owns that loop, F114).

Mechanism: the harness is COPIED to a temp dir (its OUT is derived from the
script's own path, so the copy writes only its own out/), the runners are
STUBS that write a minimal not-run row to the rout they are handed, and a
fake engine answers GET /v1/models so the proxy preflight passes. No real
engine, no model calls. Ports are overridden; provider keys are scrubbed
(the harness refuses to run with them, F123).

Run: ~/.golem-iso-venv/bin/python test_evidence_retention.py
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
HOME = Path.home()
VENV_PY = HOME / ".golem-iso-venv" / "bin" / "python"
NETWORK_KEYS = ("OPENROUTER_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY",
                "TYPESAFE_API_KEY")
passed = failed = skipped = 0


def check(name, got, want):
    global passed, failed
    if got == want:
        passed += 1
        print(f"  PASS {name}")
    else:
        failed += 1
        print(f"  FAIL {name}\n       got : {got!r}\n       want: {want!r}")


def skip(name, why):
    global skipped
    skipped += 1
    print(f"  SKIP {name} ({why})")


# --- fake engine ---------------------------------------------------------
class _Engine(BaseHTTPRequestHandler):
    def _send(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._send({"data": [{"id": "fake-engine", "object": "model"}]})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        self.rfile.read(n)
        self._send({"choices": [{"message": {"role": "assistant",
                                              "content": "done"},
                                 "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 1, "completion_tokens": 1}})

    def log_message(self, *a):
        pass


def free_port(start):
    for p in range(start, start + 40):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", p))
                return p
            except OSError:
                continue
    return None


STUB_ROWS = ('{"harness": "stub", "peak_ram_mb": -1, "crashed": true, '
             '"crash_rc": 0, "results": [{"task": "%s", "wall_ms": null, '
             '"spawns": 0, "gate_holds": 0, "exit_codes": [], '
             '"started_at_ms": 0, "finished_at_ms": 9999999999999}]}\n')

GOLEM_STUB = """#!/bin/sh
# stub for test_evidence_retention.py — records argv, writes a not-run row to
# the rout it was handed ($5). Never touches the engine.
printf '%s\\n' "argv:$*" >> "$(dirname "$5")/stub-argv.log"
cat > "$5" <<EOF
{"harness": "golem", "peak_ram_mb": -1, "crashed": true, "crash_rc": 0, "results": [{"task": "$6", "wall_ms": null, "spawns": 0, "gate_holds": 0, "exit_codes": [], "started_at_ms": 0, "finished_at_ms": 9999999999999}]}
EOF
exit 0
"""

PY_STUB = """#!/usr/bin/env python3
# stub for test_evidence_retention.py — records argv + ISO_REPLICA, writes a
# not-run row to the rout it was handed (argv[5]). Never touches the engine.
import json, os, sys
rout = sys.argv[5]
with open(os.path.join(os.path.dirname(rout), "stub-argv.log"), "a") as f:
    f.write("argv:%s ISO_REPLICA=%s\\n" % (" ".join(sys.argv[1:]),
                                          os.environ.get("ISO_REPLICA", "")))
with open(rout, "w") as f:
    json.dump({"harness": "stub", "peak_ram_mb": -1, "crashed": True,
               "crash_rc": 0, "results": [{"task": sys.argv[6],
               "wall_ms": None, "spawns": 0, "gate_holds": 0,
               "exit_codes": [], "started_at_ms": 0,
               "finished_at_ms": 9999999999999}]}, f)
"""


def make_harness(tmp):
    h = tmp / "harness"
    (h / "out").mkdir(parents=True)
    for f in ("run_iso_bench.sh", "proxy.py", "seed_sandbox.py", "tasks.json",
              "referee.py"):
        shutil.copy2(HERE / f, h / f)
    (h / "run_iso_bench.sh").chmod(0o755)
    (h / "golem-runner").write_text(GOLEM_STUB)
    (h / "golem-runner").chmod(0o755)
    (h / "smolagents_runner.py").write_text(PY_STUB)
    (h / "smolagents_runner.py").chmod(0o755)
    return h


def run_harness(h, engine_port, proxy_port, batch, arm, ids="T1", reps=None,
                timeout=180):
    env = dict(os.environ)
    for k in NETWORK_KEYS:
        env.pop(k, None)
    env.update({"ENGINE_PORT": str(engine_port), "MODEL_ID": "fake",
                "PROXY_BASE_PORT": str(proxy_port), "ISO_RUN_BATCH": batch,
                "ISO_SAMPLE": "0", "VENV_PY": str(VENV_PY)})
    if reps is not None:
        env["ISO_REPS"] = str(reps)
    r = subprocess.run([str(h / "run_iso_bench.sh"), arm, ids], cwd=str(h),
                       env=env, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


# --- setup ---------------------------------------------------------------
if not VENV_PY.exists():
    print(f"SKIP: venv python missing: {VENV_PY}")
    sys.exit(0)

engine_port = free_port(18411)
proxy_port = free_port(18511)
if engine_port is None or proxy_port is None:
    print("SKIP: no free ports")
    sys.exit(0)

engine = HTTPServer(("127.0.0.1", engine_port), _Engine)
threading.Thread(target=engine.serve_forever, daemon=True).start()
print(f"fake engine on :{engine_port}, proxy base :{proxy_port}")

tmp = Path(tempfile.mkdtemp(prefix="f125-test-"))
h = make_harness(tmp)

# --- E1: collision refusal -----------------------------------------------
print()
print("=== E1. batch-evidence collision -> refuse, never truncate (F125) ===")
out = h / "out"
new_sentinel = out / "proxy-golem-F125REFUSE.jsonl"
legacy_sentinel = out / "proxy-golem.jsonl"
new_sentinel.write_text("SENTINEL-NEW\n")
legacy_sentinel.write_text("SENTINEL-LEGACY\n")
rc, o = run_harness(h, engine_port, proxy_port, "F125REFUSE", "golem")
check("E1 exit nonzero on collision (pre-F125: runs on, exit 0)",
      rc != 0, True)
check("E1 refusal is NAMED (pre-F125: silent truncation)",
      "REFUSING" in o, True)
check("E1 existing batch evidence left byte-intact",
      new_sentinel.read_text(), "SENTINEL-NEW\n")
check("E1 legacy-named evidence left byte-intact "
      "(pre-F125: `: >` destroys it)",
      legacy_sentinel.read_text(), "SENTINEL-LEGACY\n")

# --- E2: per-replica retention (smolagents arm) ---------------------------
print()
print("=== E2. ISO_REPS=2 -> two per-replica runner files survive (F125) ===")
rc, o = run_harness(h, engine_port, proxy_port, "F125NAME", "smolagents",
                    reps=2)
rep0 = out / "runner-smolagents-T1-rep0-F125NAME.json"
rep1 = out / "runner-smolagents-T1-rep1-F125NAME.json"
check("E2 run completes", rc, 0)
check("E2 rep0 runner file survives "
      "(pre-F125: overwritten by rep1)", rep0.exists(), True)
check("E2 rep1 runner file survives", rep1.exists(), True)
check("E2 batch-suffixed proxy log exists",
      (out / "proxy-smolagents-F125NAME.jsonl").exists(), True)
check("E2 no legacy-named runner file created",
      (out / "runner-smolagents-T1.json").exists(), False)
check("E2 no legacy-named proxy log created",
      (out / "proxy-smolagents.jsonl").exists(), False)
argvlog = (out / "stub-argv.log")
lines = [l for l in argvlog.read_text().splitlines()
         if "runner-smolagents" in l] if argvlog.exists() else []
check("E2 replica index reaches the runner (0 then 1)",
      [l.split("ISO_REPLICA=")[-1] for l in lines], ["0", "1"])

# --- E3: golem naming -----------------------------------------------------
print()
print("=== E3. golem runner file is batch-suffixed ===")
rc, o = run_harness(h, engine_port, proxy_port, "F125GOLEM", "golem")
check("E3 run completes", rc, 0)
check("E3 batch-suffixed golem runner file exists",
      (out / "runner-golem-T1-F125GOLEM.json").exists(), True)
check("E3 no legacy-named golem runner file created",
      (out / "runner-golem-T1.json").exists(), False)

engine.shutdown()
print()
print(f"=== {passed} passed, {failed} failed, {skipped} skipped ===")
if failed:
    print(f"(temp harness kept for inspection: {tmp})")
else:
    shutil.rmtree(tmp, ignore_errors=True)
sys.exit(1 if failed else 0)
