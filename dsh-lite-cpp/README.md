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
include/dshlite/llm_client.hpp  Module 3a: OpenAI-compliant HTTPS client
src/llm_client.cpp              (cpp-httplib + OpenSSL, sync + async POST, strict token totals)
include/dshlite/brain.hpp       Module 3b: Executive Iteration Loop
src/brain.cpp                   (vector<Message> history, judge hook BEFORE delegation,
                                 judge timeout/failure => escalate to user, never unverified)
src/main.cpp                    dsh-lite demo binary
tests/test_{sanitizer,spawner,brain,llm}.cpp   milestone acceptance suites
tests/bench_sanitizer.cpp       10 MB / 15 ms perf gate
tests/test_llm.cpp uses an in-process loopback stub server (no network,
no keys, no TLS certs): it proves the OpenAI-compliant payload shape,
response ingestion, strict token accumulation, the async path, and the
missing-key / non-200 / malformed-JSON / bad-scheme error paths. The
TLS handshake itself is provided by OpenSSL via httplib::SSLClient and
is exercised only on the live path (./build/dsh-lite "question").

## Deps (SRS stack)

C++20, CMake >= 3.20, cpp-httplib (v0.15.3), OpenSSL, nlohmann/json
(v3.11.3). httplib/json resolve via find_package first, FetchContent
fallback second; OpenSSL via Homebrew openssl@3 on macOS.

## Build / test

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build --output-on-failure
./build/dsh-lite --offline        # no network demo
./build/dsh-lite "your question"  # live: needs OPENROUTER_API_KEY

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
