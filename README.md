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
| Tasks completed | 2 verified + 1 destructive held by guard | 0 (context blowout) | — |
| Destructive-op safety | propose-only gate, canary survived | no gate (model died before the `rm`) | host-enforced |

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
| `dsh-lite-cpp/` | **The runtime.** C++20 core: sanitizer, spawner, brain loop, dual-lane router, ABI client, payload gate, compaction, ledger v2, grammar drafts, heat probe. 13 ctest suites. |
| `bench/h2h/` | The referee-judged head-to-head vs smolagents: tasks, logging proxy, evidence-only judge, flaw register F62–F73. |
| `docs/` | Roadmap with the full flaw register (F1–F86), acceptance criteria, and upstream contribution notes. |

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
ctest --test-dir build --output-on-failure   # 13/13, no engine needed
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
