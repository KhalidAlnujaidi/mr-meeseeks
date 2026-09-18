# C++ Harness Hardening Advisories

6 advisories for the Linux porter. Each entry: location, risk, concrete fix,
acceptance check. Sources read: `include/harness.hpp` (705 lines),
`src/middleware.cpp` (776 lines), `src/sandbox.cpp` (676 lines),
`src/main.cpp` (210 lines).

---

## H1 — B^D estimator overflow clamp is UB-dependent (overflow clamp)

**File/line context:**
- `include/harness.hpp:342-346` — `estimatePreCall()` multiplies `long`
  `breadth` in a loop `parentDepth + 1` times with no overflow guard.
  Signed overflow is undefined behavior; the compiler may assume it never
  happens.
- `src/middleware.cpp:364-365` —
  `long exact = estimatePreCall(breadth, parentDepth);`
  `if (exact < 0) exact = cfg_.perTreeRequestBudget + 1; // overflow => deny`
  The clamp only works *if* overflow happens to wrap negative, which UB
  does not guarantee (may wrap positive, trap, or be folded away at -O2).

**Risk:** A pathological `breadth`/`parentDepth` (e.g. attacker-influenced
split request, or a bug passing depth 1M) can wrap to a small positive
estimate, bypassing the `BUDGET_EXCEEDED` deny and allowing an unbounded
fan-out against the shared 1000 req/day cap (`DAILY_REQUEST_CAP`,
harness.hpp:87).

**Concrete fix:** Saturate during multiplication; never let a `long`
overflow. Options (pick one):
```cpp
// in estimatePreCall, or at the middleware.cpp:364 call site:
long estimatePreCallChecked(long breadth, int parentDepth, long cap) {
  __int128 acc = 1;
  for (int i = 0; i < parentDepth + 1; ++i) {
    acc *= breadth;
    if (acc > cap) return cap + 1;  // saturate: caller denies
  }
  return (long)acc;
}
```
Call with `cap = cfg_.perTreeRequestBudget` so any estimate that would
exceed budget saturates to deny without ever overflowing `long`. Remove the
`exact < 0` heuristic. Keep the spec A.3 exactness guarantee for small
values (5^3 = 125 still reports exactly).

**Acceptance check:**
- `checkSplit("t", /*parentDepth=*/30, /*breadth=*/1000000, ...)` returns
  `Deny / BudgetExceeded` with `estimate == perTreeRequestBudget + 1`,
  under both `-O0` and `-O2 -Wall -Wextra`, on Linux g++ and Apple clang.
- UBSan (`-fsanitize=undefined`) reports no signed-overflow in the firewall
  path.

---

## H2 — Linux `isolated` flag claims isolation it does not have (Linux isolated flag)

**File/line context:**
- `src/sandbox.cpp:384-387` — `backend()` on Linux returns
  `LinuxNamespaces` whether or not `bwrap` exists (`return ...; // unshare
  path below`), so the backend name over-promises.
- `src/sandbox.cpp:416` — `res.isolated = (res.backend ==
  SandboxBackend::LinuxNamespaces);` is set *before* exec, unconditionally
  on Linux. When `bwrap` is absent and `wantSandbox` is true, no `unshare`/
  net-namespace setup actually runs — the raw `argv` executes unisolated
  but `isolated=true` is recorded (and later written to `verify.json` via
  `writeVerifyJson`, sandbox.cpp:644-660).
- The degrade-and-retry path (sandbox.cpp:548-556) correctly sets
  `isolated=false` on wrapper failure, but the no-wrapper-at-all path does
  not.

**Risk:** `verify.json` `"isolated":true` is a lie on minimal Linux hosts
(no `bwrap`, unshare unavailable in the container). Downstream gates treat
it as a real isolation signal; untrusted verify workloads run with full
net/fs access while the ledger says otherwise.

**Concrete fix:**
- Default `res.isolated = false`; set `true` only after a sandbox wrapper
  was actually prepended *and* the run succeeded without the degrade
  retry. I.e. move the assignment to after `sandboxed == true && !retry`,
  or set `res.backend = Unisolated` when `!exeExists("bwrap")` and no
  unshare wrapper is applied.
- Fix `backend()` to probe: return `LinuxNamespaces` only if `bwrap` (or a
  working `unshare`) exists, else `Unisolated`, so the `main.cpp:148`
  backend log line stops advertising namespaces on hosts that lack them.

**Acceptance check:**
- On a Linux host without `bwrap` in `PATH`, `SandboxRunner::run()` with
  `denyEgress=true` returns `isolated=false, backend=Unisolated`, and
  `verify.json` contains `"isolated":false`.
- On a host with working `bwrap`, the same call returns `isolated=true`.
- No caller reads `isolated` before `run()` returns.

---

## H3 — Pipe fd leak on partial `pipe()` failure + missing CLOEXEC (pipe fd leak)

**File/line context:**
- `src/sandbox.cpp:447-451` (`execOnce` lambda):
  ```cpp
  int outPipe[2], errPipe[2];
  if (::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
    res.exitCode = 127;
    return res;
  }
  ```
  If `::pipe(outPipe)` succeeds and `::pipe(errPipe)` fails (EMFILE/ENFILE
  under verify-storm load — exactly when fds are scarcest), the two
  `outPipe` fds are never closed: leak.
- `::fcntl(outPipe[0], F_SETFL, O_NONBLOCK)` (sandbox.cpp:480-481) sets
  non-blocking but never `FD_CLOEXEC`. The child `execvp`s the workload
  with the *read* ends still open (only dup2'd write ends are replaced),
  so every sandboxed child inherits two extra pipe fds; grandchildren
  inherit them further. Also leaks into the `bwrap`/`sandbox-exec` wrapper
  path.

**Risk:** Slow fd exhaustion in long-running hosts (one verify storm with
EMFILE leaks 2 fds per failed run, accelerating the spiral); inherited
pipe read-ends keep pipes open across `exec`, confusing EOF detection and
leaking host pipe handles into untrusted children.

**Concrete fix:**
```cpp
if (::pipe(outPipe) != 0) { res.exitCode = 127; return res; }
if (::pipe(errPipe) != 0) {
  ::close(outPipe[0]); ::close(outPipe[1]);
  res.exitCode = 127; return res;
}
// then set CLOEXEC on all four ends (or use pipe2(O_CLOEXEC) on Linux):
for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]})
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
```
Child still dup2's write ends onto stdout/stderr (dup2 clears CLOEXEC on
the *new* fd, so redirection keeps working) while read ends stay
close-on-exec.

**Acceptance check:**
- Code inspection: no `return` between the two `pipe()` calls without
  closing the first pair.
- Runtime: loop 1000 `run()` calls with a low `RLIMIT_NOFILE` (e.g. 64);
  `/proc/self/fd` count before == after (Linux) or `lsof -p` stable
  (macOS). Child `ls /dev/fd` inside the sandbox shows only 0/1/2 plus the
  intended redirection.

---

## H4 — `pmr::monotonic_buffer_resource` grows on every lookup temp key (pmr temp-key growth)

**File/line context:** `src/middleware.cpp` `TaskArenaImpl` holds
`nodes_`/`children_` as `std::pmr::unordered_map` over a single
`monotonic_buffer_resource pool_` (middleware.cpp:740-746), which *never
frees until `release()`* — and the destructor deliberately never calls
`release()` (middleware.cpp:536-541, UAF fix). Every map *lookup*
allocates a temporary `std::pmr::string` **from that same pool**:
- `enqueue`:543, `splitOnce`:558 (`find`), 641/643 (insert keys),
  644-645 (child id copies), `report`:667, `find`:711 (const method!),
  `childrenOf`:719.
- `std::pmr::string(id.data(), id.size(), &pool_)` copies the key bytes
  into monotonic storage that is never reclaimed, even though the
  temporary dies at the end of the statement.

**Risk:** Unbounded arena memory growth proportional to *query* volume, not
task count: each `find()`/`childrenOf()` poll leaks one key-sized
allocation. A long-lived host doing stall polling (`ForwardNudgeLoop::poll`
+ `arena->find` per tick) grows RSS monotonically until OOM — the
monotonic pool is effectively a leak-by-design for read traffic.

**Concrete fix (pick one):**
1. Heterogeneous lookup: give the maps a transparent hash/equal
   (`std::hash<std::string_view>` + `is_transparent`) and look up with
   `std::string_view` — zero allocation per read. (Needs `unordered_map`
   with transparent support; straightforward.)
2. Split resources: keep node storage on the monotonic pool but allocate
   *lookup temporaries* from `std::pmr::get_default_resource()` (heap,
   freed on destruction).
3. Simplest: store `std::string` keys in a plain
   `std::unordered_map<std::string, ...>` and drop pmr for the index
   (keep pmr only for bulk node blobs if profiling justifies it).

**Acceptance check:**
- Benchmark: 1M `arena->find("demo.0")` calls on a 10-node arena; RSS
  delta ≈ 0 (e.g. `< 1 MB` via `/usr/bin/time -v` or `getrusage`).
- Existing behavior preserved: `enqueue` idempotent, `splitOnce` child
  ids/parent linkage unchanged, demo (`main.cpp:106-118`) still passes.

---

## H5 — `Warn → Ok` collapse drops the BUDGET_WARN signal (Warn→Ok collapse)

**File/line context:**
- `src/middleware.cpp:391-411` — `TokenFirewallImpl::checkSplit` correctly
  returns `{Warn, BudgetWarn}` with a `budget_warn` ledger line for
  estimates in the (70%, 100%] band (spec §A.6.2 step (d)).
- `src/middleware.cpp:616-624` (`TaskArenaImpl::splitOnce`) collapses it:
  ```cpp
  if (d.verdict == GateVerdict::Deny) {
    auto code = d.code;
    if (code == GateCode::BudgetWarn) code = GateCode::Ok;  // warn allows
    if (d.code != GateCode::BudgetWarn) return {d.code, {}};
  }
  ...
  return {d.code == GateCode::BudgetWarn ? GateCode::Ok : GateCode::Ok, children};
  ```
  Both branches return `GateCode::Ok`. The arena's own ledger line does
  record `"BUDGET_WARN"` vs `"ALLOW"` (line 651), but the *return code*
  seen by the brain/host caller is always `Ok` — the warn-vs-allow
  distinction is lost at the API boundary.

**Risk:** The brain cannot tell "comfortably within budget" from "at 95%
of tree budget, prune follow-ups" (the `gateMessage(BudgetWarn)` text at
middleware.cpp:113-118 says "proceeding, prune follow-ups" — but the
caller never learns it fired except by scraping the ledger). Trees burn
into `BUDGET_EXCEEDED`/`DAILY_CAP` denies that a warn-aware scheduler
would have avoided.

**Concrete fix:** Propagate the warn. Either:
- Change `splitOnce` to return the firewall's code through:
  `return {d.code, children};` (i.e. `BudgetWarn` instead of `Ok` when the
  firewall warned), documenting that `BudgetWarn` is a non-error allow
  (matching `GateCode::BudgetWarn` = "not an error; Allow with warning",
  harness.hpp:136); or
- Return a `FirewallDecision`-style struct carrying both verdict and code.
  Update `main.cpp:112-114` demo assert (`splitCode == Ok`) to accept
  `Ok|BudgetWarn`, and audit all `splitOnce` callers for `== Ok` checks
  that must become `!= Deny`.

**Acceptance check:**
- `splitOnce` on a warn-band split (e.g. breadth 7 @ depth 1, est 49/64 —
  `main.cpp:96-104`) returns `GateCode::BudgetWarn` (not `Ok`), children
  are still enqueued, and exactly one `budget_warn` + one `BUDGET_WARN`
  split ledger line exist.
- Cheap splits still return `Ok`; over-budget still `BudgetExceeded`.

---

## H6 — POSIX-shm sister bus is declared but not implemented (shm bus future)

**File/line context:**
- `include/harness.hpp:201-251` — full shm contract: `SisterLeafShmHeader`,
  `SisterLeafMemoryBuffer` (standard-layout + trivially-copyable,
  static_asserted), `sisterLeafShmSize()`, magic `0x51534C42`, version 1,
  64-slot layout, last-writer-wins per `(parentSessionId, namespace, key)`.
- `src/middleware.cpp:754-760` — `makeSisterLeafBus(shmPath)` **ignores**
  `shmPath` and always returns `InProcessSisterLeafBus` (mutex-guarded
  `unordered_map`, middleware.cpp:247-297). Comment admits "cross-process
  futex discipline … out of scope for this milestone".
- `src/main.cpp:59` passes `""` and the demo only exercises same-process
  publish/read (main.cpp:120-132); cross-partition denial works, but
  cross-*process* sharing is untested and unimplemented.

**Risk (porter relevance):** On Linux prod the sister bus is the leaf↔leaf
IPC plane. Shipping the in-process bus silently means sisters in different
worker processes see *empty* partitions — schema anchors/contract keys
vanish across process boundaries, causing spurious re-derivation or
stale-schema use. Worse, if two processes later `mmap` the same shm name
with divergent struct versions, the `magic`/`version` check exists in the
struct but no code reads it — silent corruption instead of a clean
version-mismatch refuse.

**Concrete fix (Linux porter work items):**
1. Implement `ShmSisterLeafBus : SisterLeafBus` in a new
   `src/sister_shm.cpp`: `shm_open(shmPath, O_CREAT|O_RDWR, 0600)` +
   `ftruncate(sisterLeafShmSize())` + `mmap(MAP_SHARED)`; header init with
   `magic/version/seq` under an interprocess mutex (e.g. `pthread_mutex`
   with `PTHREAD_PROCESS_SHARED` placed in the segment, or a file lock
   around slot updates for milestone 1).
2. On attach: verify `magic == SISTER_LEAF_SHM_MAGIC &&
   version == SISTER_LEAF_SHM_VERSION`; on mismatch return an error /
   refuse attach (never silently reinterpret).
3. `publish`: `value.size() > BUS_VALUE_MAX_BYTES → BusValueTooLarge`
   (entry unchanged), stamp `timestampMs` (host clock) + `updatedBy`,
   last-writer-wins per triple, bump header `seq`; `read`/`listNamespace`
   enforce the partition scope (`parentSessionId` equality, fail-closed).
4. Keep `makeSisterLeafBus("")` → in-process (tests/macOS fallback);
   non-empty `shmPath` → shm impl on Linux. Add a two-process test: fork,
   publish in parent, read in child.

**Acceptance check:**
- Two processes opening the same `shmPath` observe each other's publishes
  within one partition and *not* across partitions.
- Oversize publish (> 64 KiB) returns `BusValueTooLarge`, segment
  unchanged.
- Attach with a corrupt/older-version magic refuses instead of reading
  garbage; `sisterLeafShmSize()` segment size matches
  `sizeof(header) + 64 * sizeof(entry)`.
- macOS build still compiles (shm impl `#ifdef HARNESS_OS_LINUX`, fallback
  to in-process).

---

*Written by reviewer for porter unblock. Verify: `wc cpp-harness/HARDENING.md`.*
