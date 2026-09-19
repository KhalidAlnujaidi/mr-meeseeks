# dsh-lite-cpp — Minimalist Budget-AGI Harness

Ultra-lightweight C++20 Brain-and-Swarm orchestrator. The Brain keeps
strategy; all labor runs in short-lived isolated worker subprocesses;
only sanitized markdown summaries cross the context firebreak.

## Layout

include/dshlite/sanitizer.hpp  Module 1: Context Firebreak & Bitstream Sanitizer
src/sanitizer.cpp               (printable ASCII + TAB/LF/CR only, ANSI stripped,
                                 truncate >4096 chars to head 2048 + marker + tail 1024)
include/dshlite/spawner.hpp     Module 2: Swarm spawner contract
src/spawner.cpp                 (fork/execve, scrubbed env, /tmp/meeseeks_<uuid>/,
                                 CLOEXEC pipes, 60 s watchdog SIGKILL, exit 124 on timeout)
include/dshlite/llm_client.hpp  Module 3a: Colibri-only HTTP client
src/llm_client.cpp              (cpp-httplib + OpenSSL, sync + async POST,
                                 verbatim --model-id, strict token totals)
include/dshlite/router.hpp      Module 3c: Multi-Engine Local Router (Gap 1)
src/router.cpp                  (role -> ordered (endpoint, model-id) pools,
                                 worker->worker fallback — never to the brain,
                                 <2-family pool warnings, isolated probe,
                                 thread-safe pooled dispatch, G3.2 velocity
                                 floor with per-entry warmup exemption)
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
include/dshlite/usage_probe.hpp P1: safe .coli_usage expert-heat reader
src/usage_probe.cpp             (route_trace.h format: v1 headers, sparse
                                 triples, legacy, IKU1 refused by magic;
                                 never throws/locks — engine publishes via
                                 temp+rename (F37); warm=mtime-fresh (F38);
                                 best-effort telemetry, gates nothing (F5))
src/main.cpp                    dsh-lite demo binary
tests/test_{sanitizer,spawner,brain,llm,router,ledger,nudge,stall,f33_p1,g4_stress}.cpp   milestone acceptance suites
tests/g4_run.cpp                Gap 4 live-run driver (needs running engines; ledger to /tmp)
tests/bench_sanitizer.cpp       10 MB / 15 ms perf gate
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
