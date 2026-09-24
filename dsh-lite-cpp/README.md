# Golem Runtime (`dsh-lite-cpp/`) — LeastGen's Native C++ Agent Runtime

Zero-dependency, native C++20 agent runtime for local & edge LLMs — the
core of [LeastGen Golem](../README.md). The Brain keeps strategy; every
piece of labor runs in a single-purpose, ephemeral worker process
spawned under strict host control (scrubbed env, pinned workspace,
watchdog kill); only sanitized summaries cross the context firebreak.
Zero-IPC execution, instant cleanup, append-only ledger.

The directory keeps its historical name (`dsh-lite-cpp`) for path
stability; the CMake project, artifacts, and docs are branded Golem.

## Architecture (complete picture)

1. **Zero-trust C++ sandbox** — every worker is a real fork/execve
   process: scrubbed envp (only the explicit allowlist), pinned
   ephemeral workspace `<temp-dir>/golem/ws_<uuid>/`, CLOEXEC pipes, 60 s
   watchdog SIGKILL. Worker output is bitstream-sanitized (Module 1)
   before the Brain ever sees it. ASan/UBSan build flags are documented
   below and used for the threaded/concurrency suites; bench-sanitizer
   (10 MB / 15 ms perf gate) is a ctest member.
2. **Dual-lane routing engine** — one ModelRouter, two transports per
   EngineEntry: native in-process C ABI decode through
   `libcolibri_segment_edge.a` (zero serve/Python/HTTP/IPC; measured
   1.33x wall-clock and +41% decode tok/s vs HTTP on local OLMoE) with
   the HTTP `coli serve` lane as fallback/peer. Lanes mix freely across
   roles (ABI brain + HTTP workers, or the reverse); ABI support is
   INJECTED (RouterConfig::abiFactory) so the core links zero Colibri;
   cross-lane fallback uses typed attempt outcomes (abi-cancelled,
   abi-context-overflow) under the same F2 worker->worker law.
3. **Host-enforced execution contract** — the pre-execution payload
   gate (schema -> allowlist -> destructive word-boundary scan;
   DESTRUCTIVE_PROPOSE_ONLY never auto-spawns), history compaction
   (8192/4096/1024, protected roles, budgeted markers), and NudgeState
   caps (maxNudgeDepth=3, MAX_ROUNDS_PER_TASK=2) all live in the C++
   host — the model can never vote itself out of them.
4. **Velocity firewalling on both lanes, one ledger** — the HTTP lane
   fires at the socket boundary (SSE no-bytes stall + G3.2 decode tok/s
   floor with warmup exemption); the ABI lane fires in-process through
   the engine's own should_cancel callback (wall-clock deadline wired
   into embed/segment_run/select — deterministic abort, no signals).
   Every turn, refusal, nudge, verify, and route decision emits
   ledger.jsonl v2: mutex-guarded single write(2), host-stamped UTC,
   honest token provenance (engine-authoritative, flagged estimates,
   or real in-process counts — never fabricated).

## Layout

include/dshlite/sanitizer.hpp  Module 1: Context Firebreak & Bitstream Sanitizer
src/sanitizer.cpp               (printable ASCII + TAB/LF/CR only, ANSI stripped,
                                 truncate >4096 chars to head 2048 + marker + tail 1024)
include/dshlite/spawner.hpp     Module 2: Swarm spawner contract
src/spawner.cpp                 (fork/execve, scrubbed env, <temp>/golem/ws_<uuid>/,
                                 CLOEXEC pipes, 60 s watchdog SIGKILL, exit 124 on timeout)
include/dshlite/llm_client.hpp  Module 3a: Colibri-only HTTP client
src/llm_client.cpp              (cpp-httplib + OpenSSL, sync + async POST,
                                 verbatim --model-id, strict token totals)
include/dshlite/abi_client.hpp  Module 3a-native: direct C ABI backend
src/abi_client.cpp              (libcolibri_segment_edge.a in-process decode,
src/abi_probe.cpp                 zero serve/Python/HTTP/IPC; opts.memory_
                                 limit_bytes wired from C++; should_cancel
                                 = host wall-clock firewall; per-call
                                 sessions (F77); typed AbiCancelledError/
                                 AbiContextOverflowError; honest in-process
                                 usage counts; F80 no chat template delta)
include/dshlite/router.hpp      Module 3c: Multi-Engine Local Router (Gap 1)
src/router.cpp                  (role -> ordered (endpoint, model-id) pools,
                                 worker->worker fallback — never to the brain,
                                 <2-family pool warnings, isolated probe,
                                 thread-safe pooled dispatch, G3.2 velocity
                                 floor with per-entry warmup exemption;
                                 MIXED LANES: EngineEntry.backend selects
                                 HTTP vs InProcessAbi via injected
                                 RouterConfig::abiFactory — core links no
                                 Colibri (F83); ABI identity abi:<modelDir>
                                 (F81), config-time factory law (F85),
                                 typed abi-cancelled/abi-context-overflow
                                 attempt outcomes fall through F2-style)
include/dshlite/ledger.hpp      Module 4: ledger.jsonl v2 emitter (Gap 3/4)
src/ledger.cpp                  (mutex + single write(2) per line (F15),
                                 ttft null when unmeasured (F11), detail
                                 always sanitized via Module 1, host-stamped
                                 UTC ts; fromRouted() fills cost block with
                                 decode-window tok/s (F13))
include/dshlite/brain.hpp       Module 3b: Executive Iteration Loop
src/brain.cpp                   (vector<Message> history, judge hook BEFORE delegation,
                                 judge timeout/failure => escalate to user, never unverified)
include/dshlite/nudge.hpp       Gap 2: nudge-depth + retry-cap state machine
src/nudge.cpp                   (G2.2 caps verbatim from harness/loop.ts:
                                 maxNudgeDepth=3 reset on user turn,
                                 MAX_ROUNDS_PER_TASK=2 then narrow/reroute/stop;
                                 G2.3 repeat-loop detection; G2.6 local
                                 verifyViaSpawn exit-code judge, F9 key scrub)
include/dshlite/payload_gate.hpp Gap 2: pre-execution payload gate (G2.4/F7)
src/payload_gate.cpp            (schema -> allowlist -> destructive word-boundary
                                 scan; DESTRUCTIVE_PROPOSE_ONLY never auto-spawns;
                                 enforceGateBeforeSpawn sequenced BEFORE
                                 SwarmSpawner::spawn — spawner stays dumb)
include/dshlite/compaction.hpp  Gap 2: history compaction + result vectors
src/compaction.cpp              (G2.5/F1: std::vector<Message>, 8192/4096/1024
                                 thresholds, protected system+final+last-user,
                                 [HOST COMPACTION] marker with reserved budget;
                                 E.2.3 result vector, BUS_VALUE_TOO_LARGE refuse-
                                 never-truncate; all text via Module 1 sanitizer)
include/dshlite/grammar.hpp     G2.4: grammar-forced tool payload drafts
src/grammar.cpp                 (colibri response_format wire contract:
                                 json_object/json_schema/gbnf; F47 local
                                 validation mirrors gateway 400s; F49/F50
                                 schema-subset laws; F45 strict payload
                                 parse — the grammar ACCELERATES drafts,
                                 the host gate stays the enforcement;
                                 F46 grammar_payload family table; F60
                                 model-id-accurate capability — glm53
                                 flash is NOT grammar-capable)
include/dshlite/usage_probe.hpp P1: safe .coli_usage expert-heat reader
src/usage_probe.cpp             (route_trace.h format: v1 headers, sparse
                                 triples, legacy, IKU1 refused by magic;
                                 never throws/locks — engine publishes via
                                 temp+rename (F37); warm=mtime-fresh (F38);
                                 best-effort telemetry, gates nothing (F5))
src/main.cpp                    Golem CLI agent (`dsh-lite`): REPL + one-shot task,
                                drives solicit -> gate -> spawn -> dual-layer
                                verify with bounded re-solicitation; derives a
                                postcondition from explicitly-stated task text
                                (F102) and extends the destructive policy (F101)
tests/test_{sanitizer,spawner,brain,llm,router,ledger,nudge,stall,f33_p1,g4_stress,grammar,abi}.cpp   milestone acceptance suites
tests/g4_run.cpp                Gap 4 live-run driver (needs running engines; ledger to
                                $GOLEM_LEDGER or <temp>/golem-g4-ledger.jsonl)
tests/abi_bench.cpp             in-process vs HTTP lane bench (manual; sequential arms per F70)
tests/bench_sanitizer.cpp       10 MB / 15 ms perf gate
test-abi offline checks are ctest-hermetic; the live engine section runs
only when ABI_MODEL_DIR is set. abi-probe/abi-bench/dshlite-abi targets
auto-disable when colibri/c/build/segment/libcolibri_segment_edge.a is
absent (make -C ../colibri/c segment-edge-library).
tests/test_llm.cpp uses an in-process loopback stub server that mimics
`coli serve` (404 unless body.model matches the served id; no keys, no
TLS): it proves the verbatim model-id payload, response ingestion,
strict token accumulation, the async path, the 404 model-id mismatch,
and the key/scheme/JSON error paths. A live engine is exercised only
via ./build/dsh-lite "question" with `coli serve` running.

## Deps (SRS stack)

C++20, CMake >= 3.20, cpp-httplib (v0.15.3), OpenSSL, nlohmann/json
(v3.11.3). httplib/json resolve via find_package first, FetchContent
fallback second; OpenSSL via Homebrew openssl@3 on macOS.

## Build / test

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build --output-on-failure
./build/dsh-lite --offline        # no engine demo
# live (Colibri-only): start the engine first, then ask:
#   coli serve --model <weights> --model-id glm-5.3-flash-colibri &
#   ./build/dsh-lite "your question"            # loopback, no key needed
#   COLI_MODEL_ID=inkling-colibri ./build/dsh-lite "your question"

ASan/UBSan (macOS note: LeakSanitizer is unsupported on this platform —
omit ASAN_OPTIONS=detect_leaks=1, which aborts every binary at startup):
-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

## Security notes

- Workers get a scrubbed envp: ONLY SpawnOptions::allowedEnv. Never put
  host keys there unless the task explicitly requires them.
- The daemon key rule from cpp-harness applies here too: judge/API keys
  travel only via the TS sidecar or explicit allowlist, never baked in.
- Workspaces persist after spawn() for forensics; call
  SwarmSpawner::cleanup(dir) when done.
