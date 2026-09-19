# SRS traceability

Each SRS requirement, where it lives, and which test proves it.

## Module 1 — Context Firebreak & Bitstream Sanitizer

| SRS clause | Implementation | Test |
|---|---|---|
| Keep 0x20-0x7E + TAB/LF/CR, drop rest | src/sanitizer.cpp fast-path span | test_sanitizer 1, 2, 12 |
| Strip ANSI escapes (SGR, cursor, OSC, charset) | src/sanitizer.cpp ESC state machine | test_sanitizer 5-8 |
| Broken multi-byte sequences dropped | bytes >= 0x80 dropped | test_sanitizer 3, 4 |
| Truncate >4096: head 2048 + marker + tail 1024 | src/sanitizer.cpp tail | test_sanitizer 9-11 |
| 10 MB in < 15 ms | memcpy span fast path | bench_sanitizer (7 ms) |
| RAII, no leaks | std::string only, no new | ASan/UBSan clean |

## Module 2 — Process Isolation

| SRS clause | Implementation | Test |
|---|---|---|
| fork()/execve(), detached memory | src/spawner.cpp | test_spawner 1 |
| Scrubbed envp[], no host keys | child gets only allowedEnv | test_spawner 2, 3 |
| /tmp/meeseeks_<uuid>/ + scope symlinks, pinned CWD | makeWorkspace + chdir | test_spawner 4 |
| Watchdog SIGKILL, default 60 s, sentinel | poll-loop + kill/waitpid | test_spawner 5 (124) |
| Sanitized summaries only to Brain | sanitize() on both streams | test_spawner 1, 5 |

## Module 3 — Brain loop & LLM client

| SRS clause | Implementation | Test |
|---|---|---|
| Async HTTPS POST, OpenAI-compliant, TLS | src/llm_client.cpp (httplib + OpenSSL) | code-reviewed; live path needs OPENROUTER_API_KEY |
| Strict token tracking | totalUsage()/requestCount() | accumulator in post() |
| vector<Message> roles system/user/assistant | include/dshlite/llm_client.hpp + brain.cpp | test_brain 1 |
| Judge hook before delegation | makeNodeJudgeHook (node harness/loop.ts judge in isolated worker) | test_brain 2 + live probe (act:do_direct, conf 0.8) |
| Judge timeout => escalate, never unverified/locked | fallback() in brain.cpp | test_brain 3-5 + live no-key probe (escalate) |

## Stack

C++20, CMake >= 3.20, cpp-httplib v0.15.3, OpenSSL (Homebrew openssl@3
on macOS), nlohmann/json v3.11.3. See CMakeLists.txt.
