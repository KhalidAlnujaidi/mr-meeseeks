// harness_c.h — stable C ABI for the TS proxy + Unix-socket daemon.
//
// The C++ classes (TokenFirewall/TaskArena/SisterLeafBus/SandboxRunner) are
// exposed as opaque handles with JSON-in/JSON-out functions so a TS sidecar
// can drive the native host without node-ffi ABI coupling. Every function is
// noexcept, returns 0 on success, and writes NUL-terminated JSON into out
// (truncated with out_truncated=1 when the buffer is short — never split a
// UTF-8 sequence: truncation backs up to the last ASCII boundary).
//
// Factoring: harness_c.cpp implements these by composing the C++ factories
// from harness.hpp. harnessd.cpp serves them over a Unix socket with a
// token check (HARNESSD_TOKEN env must match the client's first line).

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HARNESS_C_OK 0
#define HARNESS_C_ERR -1

typedef struct harness_host_s harness_host_t;

// Create/destroy the host bundle (firewall + bus + arena + sandbox).
// config_json: {"ledgerPath": "...", "shmPath": "", "repoRoot": "."}
// Returns nullptr on failure (bad JSON never crashes: defaults apply).
harness_host_t* harness_host_create(const char* config_json);
void harness_host_destroy(harness_host_t* host);

// Check a split: params_json mirrors TokenFirewall::checkSplit args.
// Out: {"verdict": "allow|warn|deny", "code": "Ok|...", "estimate": N,
//       "message": "..."}
int harness_check_split(harness_host_t* host, const char* task_id,
                        int parent_depth, int breadth, int resplit_count,
                        int parent_atomic, int parent_already_split,
                        const char* worker_models_json, long tree_used,
                        long day_used, char* out, size_t out_len,
                        int* out_truncated);

// Report a result vector (GATE-TURN). vector_json mirrors ResultVector +
// checkArtifact: {"taskId": "...", "status": "success|failed|discarded",
//                 "summary": "...", "changedFiles": [...], "gitDiffHash": "...",
//                 "exportedState": {...}, "checkArtifact": "exit=0"}
// Out: {"code": "Ok|..."}
int harness_report(harness_host_t* host, const char* vector_json, char* out,
                   size_t out_len, int* out_truncated);

// Sister bus post: entry_json mirrors SisterLeafEntry.
// Out: {"code": "Ok|BusValueTooLarge|..."}
int harness_bus_post(harness_host_t* host, const char* entry_json, char* out,
                     size_t out_len, int* out_truncated);

// Sandboxed exec: {"worktreePath": "...", "argv": [...], "timeoutMs": N,
//                  "denyEgress": true}
// Out: {"exitCode": N, "durationMs": N, "backend": "...",
//       "isolated": true|false, "stdoutTail": "...", "stderrTail": "..."}
int harness_sandbox_exec(harness_host_t* host, const char* req_json, char* out,
                         size_t out_len, int* out_truncated);

// Worktree acquire/release for the TS proxy (native git_worktree_add path).
// Out: {"code": "Ok|...", "branch": "...", "path": "...", "baseSha": "..."}
int harness_worktree_acquire(harness_host_t* host, const char* team,
                             const char* task_id, int attempt,
                             const char* base_branch, int flat_layout, char* out,
                             size_t out_len, int* out_truncated);
int harness_worktree_release(harness_host_t* host, const char* team,
                             const char* task_id, int merged, char* out,
                             size_t out_len, int* out_truncated);

// Backend label for logs: "libgit2-native" when HARNESS_USE_LIBGIT2 is set,
// else "shell-git", plus "+linux-namespaces" / "+sandbox-exec" / "+unisolated".
int harness_backend_label(char* out, size_t out_len, int* out_truncated);

// Jev decision routing (TypeSafe AI System One) — availability + budget gate.
//
// SECURITY: the daemon NEVER stores, reads, or forwards TYPESAFE_API_KEY.
// Jev inference runs in the TS sidecar (harness/jev.ts, direct fetch); the
// daemon only (a) reports routing availability/policy via harness_jev_status,
// and (b) enforces the hard B^D token budget check via harness_jev_route
// BEFORE any Jev/LLM dispatch is allowed. A deny from the budget gate means
// the caller MUST NOT dispatch to Jev/LLM for that fan-out (graceful
// fallback: stay with the local v2c hybrid, no remote call).
//
// harness_jev_status out:
//   {"available":true,"mode":"ts-sidecar-only","keyInDaemon":false,
//    "budgetGate":"B^D pre-dispatch","policy":"deny-means-no-dispatch"}
// harness_jev_route out:
//   {"verdict":"allow|warn|deny","code":"Ok|...","estimate":N,
//    "dispatchAllowed":true|false,"message":"..."} — dispatchAllowed is false
//   exactly when verdict is deny (budget gate tripped first).
int harness_jev_status(char* out, size_t out_len, int* out_truncated);
int harness_jev_route(harness_host_t* host, int parent_depth, int breadth,
                      long tree_used, long day_used, char* out, size_t out_len,
                      int* out_truncated);

#ifdef __cplusplus
}
#endif
