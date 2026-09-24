# LeastGen Golem

**Zero-Dependency, Native C++ Agent Runtime for Local & Edge LLMs.**

Golem is LeastGen's high-performance edge runtime for running autonomous
agents entirely on your own hardware. One process, no orchestration
cluster, no cloud API, no Python in the hot path. The Brain keeps
strategy; every piece of labor runs in a **single-purpose, ephemeral C++
worker** spawned under strict host control — zero-IPC execution, scrubbed
environment, instant cleanup. When a worker is done, nothing of it
remains but a line in the ledger.

Built for the regime the big frameworks were not: **small local models,
small context windows, small RAM budgets, and zero trust in model output.**

## Why Golem

Python agent frameworks assume a frontier model with a 100k+ context
window behind an API. Point one at a 1B local model with 4k context and
they fall over — transcript accumulation blows the context window, every
step pays full HTTP+SSE+JSON tax, and "verification" is the model grading
its own homework.
We ran the experiment. Same local OLMoE engine, same tasks, same token
budget, judged by an **independent referee proxy** (neither harness grades
itself) — Golem vs [smolagents](https://github.com/huggingface/smolagents)
`CodeAgent`:

| Metric (referee-logged) | **Golem** | smolagents | Delta |
|---|---|---|---|
| Prompt tokens (3 tasks) | 1,048 | 22,447 | **~21x less** |
| Avg latency per completion | 7.6 s | 137.7 s | **~18x faster** |
| Task outcomes | 1 verified (T1) + 1 content-fail (T2) + 1 destructive held by guard (T3) | 0 (harness-error on all 3) | — |
| Destructive-op safety | propose-only gate, 0 spawns, canary survived | no gate (never reached the `rm`) | host-enforced |

Read that task row carefully: this bench run does **not** show Golem
solving all three tasks. T2 is a content failure — the model appended
`Hello, budget!` instead of the required `BUDGET`, and the postcondition
caught it (F72; exit code was 0). That is the bench working as intended:
the dual-layer verdict reported a failure the old exit-code-only check
would have scored as a pass.

The smolagents column is also stated from its own error records, not
inferred: all three tasks ended in `AgentGenerationError` — T1/T2 with an
engine HTTP 500 ("the colibri engine failed to process the request") after
transcript growth, T3 with a connection error — at 733–1,349 s wall per
task. Calling that "context blowout" is a plausible reading of the 500s
but it is not what the artifacts record, so the artifacts' wording is used
here. Verify against
[`bench/h2h/out/RESULTS.md`](bench/h2h/out/RESULTS.md), with per-task
dispositions in `bench/h2h/out/ours.json` and error text in
`bench/h2h/out/theirs.json`.

Full bench, flaw register (F62–F73), and reproducible harness:
[`bench/h2h/`](bench/h2h/README.md). The headline deltas vary by run
(a lighter earlier run measured 107x prompt-token and 38x latency
advantage); the *structural* result does not move: a strict-payload,
compacting, host-verified runtime beats accumulate-everything code-agent
loops on small local models by one to two orders of magnitude.

And when Golem talks to the engine **directly through the C ABI** instead
of HTTP — in-process decode, zero serve layer, zero IPC:

| Transport | Decode tok/s | Wall clock |
|---|---|---|
| HTTP (`coli serve`) | 9.61 | baseline |
| In-process C ABI | 13.51 (**+41%**) | **1.33x faster** |

## The four load-bearing ideas

1. **Zero-trust C++ sandbox.** Workers are real `fork`/`execve` processes
   — detached memory, allowlisted `envp` (host credentials never leak
   unless explicitly passed), pinned ephemeral workspace, CLOEXEC pipes,
   watchdog `SIGKILL`, and every byte of worker output bitstream-sanitized
   before the Brain sees it. Memory safety is not a claim: the suite is
   built and run under **ASan/UBSan**.

2. **Dual-lane routing.** One router, two transports per engine entry:
   the native **in-process C ABI lane** (`libcolibri_segment_edge.a`,
   RAM-bounded engine open, wall-clock firewall wired into the engine's
   own cancel callback) and the **HTTP `coli serve` lane** (SSE streaming,
   socket-boundary TTFT). Lanes mix freely across roles — ABI brain with
   HTTP workers, or the reverse — and a failure on one lane falls through
   to the next with typed attempt outcomes. Worker fallback goes
   worker→worker, **never** to the brain.

3. **Host-enforced execution contract.** The model proposes; the host
   disposes. A pre-execution payload gate (schema → tool allowlist →
   destructive-verb scan) runs *before* any spawn — destructive ops are
   **propose-only**, recorded in the ledger, never auto-executed. History
   compaction (8192/4096/1024 char budgets, protected roles) keeps small
   contexts alive across turns. NudgeState caps (depth 3, 2 rounds per
   task) bound every retry loop. None of it is votable by the model.

4. **Verification by execution, telemetry by honesty.** Success is an
   exit code plus a **content postcondition** (F72 — exit-0 proves the
   command ran; only an artifact assertion proves it did the task), never
   a self-report. Every turn, refusal, nudge, verify, and route decision
   lands in append-only `ledger.jsonl` v2: one serialized line per event,
   mutex-guarded single `write(2)`, host-stamped UTC, and token counts
   with provenance — engine-authoritative, flagged estimates, or real
   in-process counts. Fabricated telemetry is treated as a bug.

## Repository layout

| Dir | What |
|---|---|
| `dsh-lite-cpp/` | **The runtime + CLI agent** (`dsh-lite`). C++20 core: sanitizer, spawner, brain loop, dual-lane router, ABI client, payload gate, compaction, ledger v2, grammar drafts, heat probe. 14 ctest suites. |
| `bench/h2h/` | The referee-judged head-to-head vs smolagents: tasks, logging proxy, evidence-only judge, flaw register F62–F73. |
| `bench/iso_harness/` | The iso-model benchmark: same engine, same model, same tasks — only the harness varies. 10 sealed tasks, F72 dual-layer referee, three adapters, flaw register F87–F100. |
| `docs/` | Roadmap with the flaw register (F1–F85), acceptance criteria, and upstream contribution notes. |

### CLI agent

`dsh-lite` is the agent entry point: the model proposes, the host
disposes, and nothing is called done on the model's say-so.

```sh
./build/dsh-lite "create a file note.txt containing exactly the text ISO-ATOM"
./build/dsh-lite                 # interactive REPL
./build/dsh-lite --offline       # stack self-demo, no engine
```

Each turn runs the full stack: strict-parse solicitation → pre-execution
payload gate → sandboxed ephemeral spawn → **dual-layer verification**
(exit code AND a content postcondition derived from the task text) →
bounded retry with host-driven re-solicitation. Outcomes are reported with
the runtime's honest labels: `verified` means both layers held;
`verified-exit-only` means it ran but the task stated nothing checkable;
`DESTRUCTIVE_PROPOSE_ONLY` means the gate held it with zero spawns.

Current status is stated rather than implied: the iso arm has only the
`golem` column filled so far (`smolagents`/`langgraph` marked `not-run`,
never imputed), and its latest full-suite run scores golem **5/10**
(3 verified, 2 guard-held, 5 honest model failures). The cross-harness
headline in the table above comes from the older `bench/h2h/` bench, not
from `bench/iso_harness/`. See
[`bench/iso_harness/out/ISO_RESULTS.md`](bench/iso_harness/out/ISO_RESULTS.md)
for the per-task audit trail.

**Known limitation (F100).** On this engine the model frequently emits a
*command name* where a tool name is required (`echo`, `sed`, `cp`, `sort`,
`cat`, `wc`, `sh`, `mkdir` — 10 distinct observed over 10 tasks × 3
repeats), so the pre-execution allowlist refuses intent that was merely
mis-labelled. Measured as a model-capability result, not a harness defect,
and deliberately not absorbed by widening the policy — see F100 in
[`bench/iso_harness/README.md`](bench/iso_harness/README.md) for why
allowlisting command names would weaken the safety tasks.

(The repo also carries earlier TypeScript-era components under
`plugin/`, `preset/`, `scripts/`, `cpp-harness/` — see git history. The
Golem runtime is `dsh-lite-cpp/`.)

## Quick start

Requirements: C++20 compiler, CMake ≥ 3.20, OpenSSL, nlohmann/json,
cpp-httplib (the last two auto-resolve via FetchContent if not
installed). A local [Colibri](https://github.com/JustVugg/colibri)
engine + model weights for live runs; the offline test suite needs none
of it.

```sh
cd dsh-lite-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build --output-on-failure   # 14/14, no engine needed
```

In-process C ABI lane (optional, needs the Colibri static lib):

```sh
make -C ../colibri/c segment-edge-library    # -> libcolibri_segment_edge.a
cmake --build build --target abi-probe mixed-lane-run
./build/abi-probe /path/to/model 24 "The capital of France is"
```

See [`dsh-lite-cpp/README.md`](dsh-lite-cpp/README.md) for the full
module map and [`CONTRIBUTING.md`](CONTRIBUTING.md) for the build law.

## License

MIT — see [LICENSE](LICENSE). © LeastGen.

Security reports: [SECURITY.md](SECURITY.md) (private disclosure, please).
