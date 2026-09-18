// harness.hpp — C++20 Brain-and-Swarm host middleware contracts.
//
// Covers spec docs/budget-agi-host-middleware-spec.md:
//   Part A  Token Firewall + Depth Middleware  (I1..I10, B^D estimator)
//   Part B  Git Worktree Isolation Layer
//   Part D  Token Guard pre-call proxy, Auto-Nudge Loop, Verification Pipeline
//   Part E  Sister-leaf KV bus + compaction/bubbling pipeline (result vector)
//
// Header-only contracts: structs, enums, abstract interfaces. Host- enforced
// semantics live in middleware.cpp / sandbox.cpp / main.cpp (engineer tasks).
// This header must compile clean under -std=c++20 -Wall -Wextra on both
// Linux (g++ / clang++) and macOS (Apple clang).
//
// Memory model recap (memory/memory.ts):
//   L0 harness: host, OS, budgets — nothing project-specific.
//   L1 project: persistent project brain, shared across sessions.
//   L2 session: caller-owned scratch, never auto-promoted.
//   Ln leaf:    ephemeral task-scoped context, compacted to a result vector
//              before control returns to L2; raw scratch is purged.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// §0 Platform guards (portable Linux + macOS)
// ---------------------------------------------------------------------------

#if defined(__linux__)
#define HARNESS_OS_LINUX 1
#else
#define HARNESS_OS_LINUX 0
#endif

#if defined(__APPLE__) && defined(__MACH__)
#define HARNESS_OS_MACOS 1
#else
#define HARNESS_OS_MACOS 0
#endif

#if !HARNESS_OS_LINUX && !HARNESS_OS_MACOS
#error "harness.hpp supports only Linux and macOS"
#endif

// Sandbox backend availability. Linux prefers namespaces/seccomp/bubblewrap;
// macOS falls back to sandbox-exec (best-effort isolation, never a gate).
#if HARNESS_OS_LINUX
#define HARNESS_HAS_NAMESPACES 1
#define HARNESS_HAS_SECCOMP 1
#else
#define HARNESS_HAS_NAMESPACES 0
#define HARNESS_HAS_SECCOMP 0
#endif

#if HARNESS_OS_MACOS
#define HARNESS_HAS_SANDBOX_EXEC 1
#else
#define HARNESS_HAS_SANDBOX_EXEC 0
#endif

// Interprocess sync primitives live in the shm file header (Linux only;
// macOS has no PTHREAD_MUTEX_ROBUST, so the header carries padding there).
#if HARNESS_OS_LINUX
#include <pthread.h>
#include <semaphore.h>
#endif

// POSIX shared-memory primitive per platform: shm_open(3) on Linux,
// shm_open(3) (deprecated-but-working) on macOS. Anonymous mmap(2) is the
// fallback when a named segment cannot be created.
#if HARNESS_OS_LINUX || HARNESS_OS_MACOS
#define HARNESS_HAS_POSIX_SHM 1
#else
#define HARNESS_HAS_POSIX_SHM 0
#endif

namespace harness {

// ---------------------------------------------------------------------------
// §1 Global budget constants (spec §A.2 / §D.1 — HOST-ENFORCED)
// ---------------------------------------------------------------------------

/// Shared OpenRouter free-tier request cap across ALL sessions/keys/trees.
inline constexpr int DAILY_REQUEST_CAP = 1000;
/// Max agents one delegation tree may ever cost (== DEFAULT_BUDGET_MAX_AGENTS).
inline constexpr int PER_TREE_REQUEST_BUDGET = 64;
/// Max children any single split may create.
inline constexpr int PER_DEPTH_BREADTH_CAP = 8;
/// Soft warn at 70% of per-tree budget, hard deny above 100%.
inline constexpr double SOFT_WARN_RATIO = 0.7;
inline constexpr double HARD_DENY_RATIO = 1.0;

/// I1: members may delegate at most this deep (DSH stock default 1;
/// 0 = members may not delegate at all).
inline constexpr int MEMBER_MAX_DEPTH = 1;
/// I7: max chained auto-continuations without an intervening user turn.
inline constexpr int MAX_NUDGE_DEPTH = 3;
/// I5: max parallel continuations per report (prune to top-5, defer rest).
inline constexpr int MAX_PARALLEL_PATHS = 5;
/// I6: max verification retries per task; 3rd attempt must be narrower.
inline constexpr int MAX_ROUNDS_PER_TASK = 2;
/// I4/SPIRAL: same lineage re-split >= 3x is a decomposition bug.
inline constexpr int MAX_RESPLITS = 3;

/// §E.1.3: per-entry serialized value cap on the sister bus.
inline constexpr std::size_t BUS_VALUE_MAX_BYTES = 65536;
/// §E.2.3: exportedState serialized cap (oversize => deny, never truncate).
inline constexpr std::size_t EXPORTED_STATE_MAX_BYTES = 4096;
/// §E.2.2: structural pruning thresholds (reuse cordis tool-result-pruner).
inline constexpr std::size_t PRUNE_THRESHOLD_CHARS = 8192;
inline constexpr std::size_t PRUNE_HEAD_CHARS = 4096;
inline constexpr std::size_t PRUNE_TAIL_CHARS = 1024;
/// §D.2: stall detection wall-clock (host-measured, never LLM self-report).
inline constexpr std::chrono::milliseconds STALL_AFTER_MS{120000};
/// §D.4: verification pipeline default timeout inside the worktree.
inline constexpr std::chrono::milliseconds VERIFY_TIMEOUT_MS{300000};

// ---------------------------------------------------------------------------
// §2 Gate verdicts + error codes (spec §A.7 / §D.5 / §E.3)
// ---------------------------------------------------------------------------

/// Machine-readable gate outcome. Fail-closed: Deny changes no state
/// except appending one DENY ledger line.
enum class GateVerdict {
  Allow,    ///< estimate within budget, all caps hold.
  Warn,     ///< allow + BUDGET_WARN ledger line + brain-visible warning.
  Deny,     ///< refused; see GateCode for why and how to recover.
  ProposeOnly,  ///< destructive verbs: brain may propose, never auto-enqueue.
};

enum class GateCode {
  Ok = 0,
  BudgetWarn,             ///< BUDGET_WARN (not an error; Allow with warning)
  SpiralStop,             ///< SPIRAL_STOP: resplit_count reached 3
  BudgetExceeded,         ///< BUDGET_EXCEEDED: breadth^depth > tree budget
  BreadthCap,             ///< BREADTH_CAP: breadth > per-depth cap
  DailyCap,               ///< DAILY_CAP: shared 1000/day exhausted
  DepthFirebreak,         ///< DEPTH_FIREBREAK: depth > memberMaxDepth
  HomogeneousAssignment,  ///< HOMO_ASSIGNMENT: multi-child split, <2 models
  DestructiveProposeOnly, ///< DESTRUCTIVE_PROPOSE_ONLY: needs user approval
  MaxRounds,              ///< MAX_ROUNDS: failed verification twice
  AtomicLocked,           ///< I8: atomic-flagged task rejects children
  SplitOnce,              ///< I3: same parent id split twice
  BusValueTooLarge,       ///< BUS_VALUE_TOO_LARGE: entry/exportedState over cap
  ArtifactsPending,       ///< E_ARTIFACTS_PENDING: release before artifacts done
  VerifyFail,             ///< V_VERIFY_FAIL: non-zero harness exit
  VerifyTimeout,          ///< V_VERIFY_TIMEOUT: killed at timeoutMs
  HarnessAbsent,          ///< V_HARNESS_ABSENT: no harness -> waiver path
  WorktreeMainDirty,      ///< WT_MAIN_DIRTY
  WorktreeStaleBase,      ///< WT_STALE_BASE
  WorktreeLocked,         ///< WT_LOCKED / WT_BROKEN
  WorktreeBranchCheckedOut,  ///< WT_BRANCH_CHECKED_OUT
  WorktreePathExists,     ///< WT_PATH_EXISTS
  WorktreeMergeConflict,  ///< WT_MERGE_CONFLICT
  WorktreeUntrackedOnly,  ///< WT_UNTRACKED_ONLY
  WorktreeGitignoreMissing,  ///< WT_GITIGNORE_MISSING (flat layout needs it)
};

/// Exact brain-visible message per code (spec §A.7 normative wording).
std::string gateMessage(GateCode code, const std::string& taskId,
                        long estimate = 0, long budget = 0);

// ---------------------------------------------------------------------------
// §3 Memory tiers L0 / L1 / L2 / Ln (memory/memory.ts + spec §E)
// ---------------------------------------------------------------------------

/// Layered-memory tier of a stored item.
enum class MemoryTier : std::uint8_t {
  L0 = 0,  ///< Harness-global (host, OS, budgets). Never project-specific.
  L1 = 1,  ///< Project brain, shared across sessions. Explicit approve only.
  L2 = 2,  ///< Session scratch, caller-owned. Never auto-promotes.
  Ln = 3,  ///< Ephemeral leaf context. Purged at release; only the result
           ///< vector survives (spec §E.2.4).
};

/// Visibility of a sister-bus entry (spec §E.1.4 — HOST-ENFORCED).
/// sister_only NEVER promotes, never surfaces in L2 vectors.
/// promotable_to_l1 is a CANDIDATE only: must still pass proposePromotion()
/// secret screening + explicit BRAIN approve(file). Auto-merge is FORBIDDEN.
enum class Visibility : std::uint8_t {
  SisterOnly = 0,
  PromotableToL1 = 1,
};

// ---------------------------------------------------------------------------
// §4 SisterLeafMemoryBuffer — fixed C-struct for POSIX shm (spec §E.1)
// ---------------------------------------------------------------------------
//
// Layout rules (both platforms, both compilers):
//   * standard-layout + trivially copyable (static_asserted below)
//   * fixed-size char arrays only — no std::string, no pointers, so the
//     struct is meaningful when mmap'ed at different base addresses
//   * little-endian hosts assumed (x86-64 / arm64); magic field detects
//     accidental cross-endian mapping
//   * payload cap BUS_VALUE_MAX_BYTES (64 KiB) keeps the bus an IPC state
//     layer, never a blob store (spec §E.1.3)
//
// One shm segment holds a file header (SisterLeafShmHeader) followed by
// MAX_ENTRIES slots. Publish = last-writer-wins per
// (parentSessionId, namespace, key) within one partition.

inline constexpr std::uint32_t SISTER_LEAF_SHM_MAGIC = 0x51534C42u;  // 'QSLB'
inline constexpr std::uint16_t SISTER_LEAF_SHM_VERSION = 1;
inline constexpr std::size_t SISTER_TASK_ID_MAX = 64;
inline constexpr std::size_t SISTER_SESSION_ID_MAX = 64;
inline constexpr std::size_t SISTER_NAMESPACE_MAX = 64;
inline constexpr std::size_t SISTER_KEY_MAX = 128;
inline constexpr std::size_t SISTER_WORKER_ID_MAX = 64;
inline constexpr std::size_t SISTER_VALUE_MAX = BUS_VALUE_MAX_BYTES;
inline constexpr std::size_t SISTER_SHM_MAX_ENTRIES = 64;

struct SisterLeafMemoryBuffer {
  std::uint32_t magic;  ///< == SISTER_LEAF_SHM_MAGIC (endian/validity check)
  std::uint16_t version;         ///< == SISTER_LEAF_SHM_VERSION
  std::uint8_t visibility;       ///< Visibility as raw byte (0/1)
  std::uint8_t reserved;         ///< alignment padding, must be 0
  std::uint64_t timestampMs;     ///< host-assigned ms epoch (worker values
                                 ///< ignored — spec §E.1.3)
  char taskId[SISTER_TASK_ID_MAX];          ///< sub-task id within the DAG
  char parentSessionId[SISTER_SESSION_ID_MAX];  ///< partition key (E.1.2)
  char namespace_[SISTER_NAMESPACE_MAX];    ///< e.g. "types", "api_schema"
  char key[SISTER_KEY_MAX];
  char updatedBy[SISTER_WORKER_ID_MAX];  ///< host-stamped worker instance id
  std::uint32_t valueLen;  ///< valid bytes in value[] (<= SISTER_VALUE_MAX)
  char value[SISTER_VALUE_MAX];  ///< serialized payload (JSON or raw string)
};

static_assert(std::is_standard_layout_v<SisterLeafMemoryBuffer>,
              "SisterLeafMemoryBuffer must be standard-layout for shm");
static_assert(std::is_trivially_copyable_v<SisterLeafMemoryBuffer>,
              "SisterLeafMemoryBuffer must be trivially copyable for shm");

/// File header at offset 0 of the shm segment, followed by the slot array.
/// On Linux the header embeds a process-shared ROBUST mutex + a publish
/// semaphore at fixed offsets so two worker processes synchronize through
/// the segment itself. macOS keeps the same size via padding (no robust
/// mutex support); the shm bus is Linux-only there (H6).
struct SisterLeafShmHeader {
  std::uint32_t magic;      ///< == SISTER_LEAF_SHM_MAGIC
  std::uint16_t version;    ///< == SISTER_LEAF_SHM_VERSION
  std::uint16_t entryCount;         ///< live slots (<= SISTER_SHM_MAX_ENTRIES)
  std::uint64_t seq;                ///< host-incremented publish counter
#if HARNESS_OS_LINUX
  pthread_mutex_t lock;  ///< PTHREAD_PROCESS_SHARED | PTHREAD_MUTEX_ROBUST
  sem_t publishSem;      ///< process-shared, counts committed publishes
#else
  // Size parity with the Linux layout (mutex 40B + sem 32B on glibc x86-64).
  std::array<char, 72> ipcPad{};
#endif
};

static_assert(std::is_standard_layout_v<SisterLeafShmHeader>);
// NOTE: no trivially-copyable assert on the header: the Linux ipc members
// (pthread_mutex_t/sem_t) are plain POD on glibc but the guarantee is not
// portable. Slots (SisterLeafMemoryBuffer) keep the strict shm asserts.

/// Total mapped size of one bus segment (header + slots).
inline constexpr std::size_t sisterLeafShmSize() {
  return sizeof(SisterLeafShmHeader) +
         SISTER_SHM_MAX_ENTRIES * sizeof(SisterLeafMemoryBuffer);
}

// ---------------------------------------------------------------------------
// §5 SisterLeafBus — task-scoped leaf<->leaf IPC (spec §E.1)
// ---------------------------------------------------------------------------

/// C++ view of one bus entry (owned strings; convert to/from the shm
/// fixed struct at the segment boundary).
struct SisterLeafEntry {
  std::string taskId;
  std::string parentSessionId;  ///< partition key
  std::string namespace_;
  std::string key;
  std::string value;  ///< serialized, <= BUS_VALUE_MAX_BYTES
  Visibility visibility = Visibility::SisterOnly;
  std::string updatedBy;    ///< filled by host, not by the caller
  std::uint64_t timestampMs = 0;  ///< filled by host at publish
};

/// Abstract bus. Implementations: POSIX-shm segment (prod) + in-process
/// map (tests / macOS fallback when shm_open is unavailable).
class SisterLeafBus {
 public:
  virtual ~SisterLeafBus() = default;

  /// Last-writer-wins per (parentSessionId, namespace, key).
  /// Returns BusValueTooLarge when serialized value > 64 KiB (entry
  /// unchanged). Host stamps timestampMs + updatedBy.
  virtual GateCode publish(const SisterLeafEntry& entry) = 0;

  /// Partition-scoped read: only the caller's own parentSessionId
  /// partition is visible; cross-partition reads return nullopt
  /// (fail-closed, spec §E.1.2).
  virtual std::optional<SisterLeafEntry> read(
      const std::string& parentSessionId, const std::string& namespace_,
      const std::string& key) = 0;

  /// All entries of one namespace within one partition.
  virtual std::map<std::string, SisterLeafEntry> listNamespace(
      const std::string& parentSessionId, const std::string& namespace_) = 0;
};

// ---------------------------------------------------------------------------
// §6 ResultVector — L2-facing compaction output (spec §E.2.3 — NEW)
// ---------------------------------------------------------------------------

/// Canonical task status. Lowercase worker input is normalized to these;
/// anything else rejects the vector (spec §E.2.3).
enum class TaskStatus : std::uint8_t {
  Success = 0,  ///< SUCCESS == proposed then merged (patch accepted)
  Failed = 1,   ///< FAILED == discarded, fail terminal (B.5 forensics)
  Discarded = 2,  ///< DISCARDED == discarded via cancel/stale path
};

/// The host forces every leaf context into this 1–5 line vector.
/// Only this (plus patch + verify.json + ledger lines) survives release.
struct ResultVector {
  std::string taskId;  ///< == originating task id (required)
  TaskStatus status = TaskStatus::Success;
  /// 1–5 lines, no stack traces / raw logs (those live in patch + pruned
  /// transcript). Carries the no-patch reason when gitDiffHash is empty.
  std::string summary;
  std::vector<std::string> changedFiles;  ///< repo-relative paths, may be empty
  /// Short hash of patches/<task>.patch. REQUIRED iff the patch exists
  /// (binds vector to patch); "" with reason-in-summary when B.5/D.4
  /// withhold the patch (FAILED/DISCARDED, keepOnFail=false or
  /// V_VERIFY_FAIL).
  std::string gitDiffHash;
  /// Leaf-declared contract keys for downstream sisters (header names,
  /// schema anchors...). Scalar strings only, <= 4 KiB serialized.
  /// sister_only bus content and secrets MUST NOT appear here.
  std::map<std::string, std::string> exportedState;

  /// Validate field bounds + patch binding. Returns Ok or the refusing
  /// code (BusValueTooLarge for oversize/non-scalar exportedState, …).
  GateCode validate(bool patchExists) const;
  /// "SUCCESS" | "FAILED" | "DISCARDED" (uppercase L2-facing convention).
  std::string statusString() const;
  /// Normalize worker spelling ("success" -> Success); nullopt = reject.
  static std::optional<TaskStatus> parseStatus(const std::string& s);
};

// ---------------------------------------------------------------------------
// §7 TokenFirewall — B^D pre-call proxy (spec §A.3 / §D.1 — HOST-ENFORCED)
// ---------------------------------------------------------------------------

/// Normative estimator, exactly harness/loop.ts:630:
///   estimate = breadth^(parentDepth + 1)
/// breadth = proposed children count; parentDepth = splitting task depth
/// (root 0). Deliberately pessimistic: worst case if every child re-splits
/// at the same breadth.
///
/// H1: plain `estimatePreCall` can overflow `long` (signed UB). Gate code
/// MUST use `estimatePreCallChecked` with a saturation cap instead; the raw
/// form is kept for exact small-value reporting only (5^3 = 125).
inline constexpr long estimatePreCall(long breadth, int parentDepth) {
  long estimate = 1;
  for (int i = 0; i < parentDepth + 1; ++i) estimate *= breadth;
  return estimate;
}

/// Saturating estimator (H1 fix): accumulates in __int128. Values that fit
/// in `long` report EXACTLY (spec A.3: 5^3 = 125); only products beyond
/// LONG_MAX saturate to cap+1 (caller denies). Never overflows `long`, so
/// no `exact < 0` heuristic and no signed-UB at any -O level.
inline long estimatePreCallChecked(long breadth, int parentDepth, long cap) {
  __int128 acc = 1;
  for (int i = 0; i < parentDepth + 1; ++i) {
    acc *= breadth;
    if (acc > static_cast<__int128>(__LONG_MAX__)) return cap + 1;
  }
  return static_cast<long>(acc);
}

struct TokenFirewallConfig {
  int dailyRequestCap = DAILY_REQUEST_CAP;  ///< global, shared, not per-tree
  int perTreeRequestBudget = PER_TREE_REQUEST_BUDGET;
  int perDepthBreadthCap = PER_DEPTH_BREADTH_CAP;
  double softWarnRatio = SOFT_WARN_RATIO;
  double hardDenyRatio = HARD_DENY_RATIO;
  std::string ledgerPath;  ///< JSONL ledger; every decision appends a line
};

struct FirewallDecision {
  GateVerdict verdict = GateVerdict::Allow;
  GateCode code = GateCode::Ok;
  long estimate = 0;
  long dayUsed = 0;
  std::string message;  ///< brain-visible, names cap + numbers + recovery
};

class TokenFirewall {
 public:
  explicit TokenFirewall(TokenFirewallConfig cfg);
  virtual ~TokenFirewall() = default;

  /// GATE-SPAWN pre-call (breadth = 1): memberMaxDepth firebreak (I1),
  /// shared pre-call daily-cap check (I2), destructive-verb scan.
  virtual FirewallDecision checkSpawn(const std::string& taskId, int childDepth,
                                      long dayUsed) = 0;

  /// GATE-SPLIT (fan-out): normative order — (a) spiral I4, (b) hetero
  /// assignment, (c) breadth cap, (d) B^D vs per-tree budget warn/deny,
  /// (e) daily cap, (f) split-once/atomic-lock (spec §A.6.2).
  virtual FirewallDecision checkSplit(const std::string& taskId,
                                      int parentDepth, int breadth,
                                      int resplitCount, bool parentAtomic,
                                      bool parentAlreadySplit,
                                      const std::vector<std::string>& workerModels,
                                      long treeUsed, long dayUsed) = 0;

  /// Record one request-type ledger line (drives dayUsed accounting).
  virtual void recordRequest(const std::string& taskId) = 0;
  virtual long dayUsed() const = 0;
};

// ---------------------------------------------------------------------------
// §8 ForwardNudgeLoop — stall/yield detect + re-prompt (spec §D.2)
// ---------------------------------------------------------------------------
//
// Extends the passive I7 per-turn counter (maxNudgeDepth=3, reset on user
// turn) with an ACTIVE host loop: stall detection -> re-prompt within the
// shared nudge budget -> fallback routing (narrower / different model /
// stop+report). Interrupt keywords cancel all pending nudges + resume-lock.

struct NudgeConfig {
  int maxNudgeDepth = MAX_NUDGE_DEPTH;  ///< shared I7 budget (re-prompts AND
                                        ///< member auto-continues count)
  int maxParallelPaths = MAX_PARALLEL_PATHS;
  int maxRoundsPerTask = MAX_ROUNDS_PER_TASK;
  std::chrono::milliseconds stallAfterMs = STALL_AFTER_MS;
  bool nudgeOnYield = true;
  std::string fallbackModel;  ///< hetero re-route target (optional)
};

enum class NudgeReason : std::uint8_t {
  Stall = 0,  ///< no ledger line from owner for stallAfterMs
  YieldFail = 1,  ///< withheld auto-continue short of goal / verify false
};

struct NudgeContext {
  int nudgeDepth = 0;  ///< increments per host re-prompt AND auto-continue
  NudgeReason reason = NudgeReason::Stall;
  std::string lastDetail;
};

enum class NudgeAction : std::uint8_t {
  Reprompt = 0,  ///< re-queue SAME (task, attempt) + nudge context line
  EscalateNarrower = 1,  ///< re-scope to a narrower criterion
  EscalateDifferentModel = 2,  ///< reassign to fallbackModel (hetero rule)
  StopAndReport = 3,  ///< nudge budget exhausted: plain report, withheld 3/3
  WithheldInterrupt = 4,  ///< user stop/pause/hold: cancel + resume-lock
};

class ForwardNudgeLoop {
 public:
  virtual ~ForwardNudgeLoop() = default;

  /// Any tool call / turn completion resets the wall-clock stall timer.
  virtual void heartbeat(const std::string& taskId,
                         std::chrono::steady_clock::time_point now) = 0;

  /// Evaluate one active (task, attempt): stall (> stallAfterMs since last
  /// ledger line) or yield (withheld report / verify-false with retries
  /// left) -> Reprompt within budget; 3rd consecutive stall, budget
  /// exhausted (3/3), or MAX_ROUNDS -> escalate/stop, never full-scope
  /// retry. User interrupt always wins -> WithheldInterrupt.
  virtual NudgeAction poll(const std::string& taskId,
                           const NudgeContext& nudge,
                           bool userInterrupted, int consecutiveStalls,
                           int verifyFailures,
                           std::chrono::steady_clock::time_point now) = 0;

  /// Enforce the per-report parallel cap: keep top-5 by value, remainder
  /// as Deferred: lines (spec §A.4).
  virtual std::pair<std::vector<std::string>, std::vector<std::string>>
  pruneParallelPaths(std::vector<std::string> proposed) = 0;
};

// ---------------------------------------------------------------------------
// §9 WorktreeManager — git worktree isolation (spec §B / §D.3)
// ---------------------------------------------------------------------------

/// Naming layout. Canonical is team-scoped; flat is a supported alias
/// (single-team-per-repo only — flat collides across teams, spec §D.3).
enum class WorktreeLayout : std::uint8_t {
  Team = 0,  ///< task/<team>/<task-id> @ .agent-teams/<team>/worktrees/<id>/
  Flat = 1,  ///< agent/<task-id> @ .worktrees/<task-id>/ (+ .gitignore gate)
};

enum class WorktreeState : std::uint8_t {
  Active = 0,
  Proposed = 1,  ///< patch written, awaiting BRAIN review
  Merged = 2,
  Discarded = 3,
  Stale = 4,  ///< base moved beyond threshold -> rebase-or-reacquire
};

struct WorktreeHandle {
  std::string taskId;
  int attempt = 0;
  std::string branch;  ///< host-minted; workers never choose names
  std::string path;    ///< member CWD pinned here for the whole attempt
  std::string baseBranch = "main";
  std::string baseSha;  ///< pinned at acquire; branch cut from this SHA
  WorktreeState state = WorktreeState::Active;
};

struct ProposeResult {
  std::string patchFile;  ///< patches/<task>.patch (what BRAIN reviews)
  std::string stat;       ///< diff --stat vs baseSha
  std::string statusPorcelain;
  std::string verifyJsonPath;  ///< D.4 pipeline artifact
};

enum class MergeStrategy : std::uint8_t {
  Apply = 0,
  MergeFf = 1,
  MergeNoFf = 2,
};

class WorktreeManager {
 public:
  virtual ~WorktreeManager() = default;

  /// Mint branch+path, `git worktree add -b <branch> <path> <baseSha>`,
  /// registry insert active. Denied when budget/depth firewalls trip
  /// (no worktree, no spend) or on WT_* preconditions.
  virtual std::pair<GateCode, WorktreeHandle> acquire(
      const std::string& team, const std::string& taskId, int attempt,
      const std::string& baseBranch = "main",
      WorktreeLayout layout = WorktreeLayout::Team) = 0;

  /// Run D.4 verification pipeline INSIDE the worktree, then write the
  /// patch. Red suite -> registry stays active, V_VERIFY_FAIL, no patch
  /// (BRAIN never reviews red code).
  virtual std::pair<GateCode, ProposeResult> propose(
      const std::string& team, const std::string& taskId) = 0;

  /// BRAIN/captain only. apply --check -> apply/merge, then pre-flight
  /// (build green, one API regression, secret grep clean). Serialized by
  /// host mutex (spec §B.4).
  virtual std::pair<GateCode, std::string> mergeBack(
      const std::string& team, const std::string& taskId,
      MergeStrategy strategy) = 0;

  /// remove --force + prune + branch -D (unless kept); tombstone registry.
  virtual GateCode release(const std::string& team, const std::string& taskId,
                           bool merged, bool keepBranch = false) = 0;

  /// Registry joined with `git worktree list --porcelain`. Paths are
  /// ALWAYS resolved via list()/registry, never string concatenation
  /// (mixed-layout safety, spec §D.3).
  virtual std::vector<WorktreeHandle> list(const std::string& team) = 0;
};

// ---------------------------------------------------------------------------
// §10 SandboxRunner — verify/test execution inside the worktree (spec §D.4)
// ---------------------------------------------------------------------------

/// Backend actually used for one run. Linux: namespaces/seccomp isolation;
/// macOS: sandbox-exec best-effort (isolation failure never blocks
/// `proposed` — only exit codes gate; recorded as isolated=false).
enum class SandboxBackend : std::uint8_t {
  LinuxNamespaces = 0,
  MacSandboxExec = 1,
  Unisolated = 2,  ///< fallback; run proceeds, verify.json notes it
};

/// Which harness the host detected in the worktree.
enum class VerifyHarness : std::uint8_t {
  None = 0,  ///< -> waiver path, never silent-pass (V_HARNESS_ABSENT)
  NpmTest = 1,
  NpmVerify = 2,  ///< `npm run verify` when `test` is absent
  Pytest = 3,
  Eslint = 4,
  Ruff = 5,
};

enum class VerifyVerdict : std::uint8_t {
  Pass = 0,           ///< exit 0 on ALL detected harnesses
  Fail = 1,           ///< V_VERIFY_FAIL — patch withheld, worker retries
  Timeout = 2,        ///< V_VERIFY_TIMEOUT — killed at timeoutMs
  WaivedDocsOnly = 3,  ///< md-only diff + BRAIN waiver line required
  WaivedHarnessAbsent = 4,  ///< no harness + BRAIN waiver line required
};

struct SandboxRunRequest {
  std::string worktreePath;  ///< CWD pinned here; writes outside denied
  std::vector<std::string> argv;  ///< exact argv recorded in verify.json
  std::chrono::milliseconds timeoutMs = VERIFY_TIMEOUT_MS;
  bool denyEgress = true;  ///< best-effort (PROMPT); failure never blocks
};

struct SandboxRunResult {
  int exitCode = 0;
  long durationMs = 0;
  std::string stdoutTail;  ///< pruned head+tail (E.2.2 thresholds)
  std::string stderrTail;
  SandboxBackend backend = SandboxBackend::Unisolated;
  bool isolated = false;
};

struct VerifyReport {
  VerifyVerdict verdict = VerifyVerdict::Fail;
  std::vector<VerifyHarness> harnesses;
  std::vector<int> exitCodes;
  long durationMs = 0;
  bool isolated = false;
  std::string verifyJsonPath;
};

class SandboxRunner {
 public:
  virtual ~SandboxRunner() = default;

  /// Execute one argv inside the worktree sandbox. Platform dispatch:
  /// lavoro namespaces/seccomp on Linux, sandbox-exec on macOS,
  /// unisolated fallback with isolated=false recorded.
  virtual SandboxRunResult run(const SandboxRunRequest& req) = 0;

  /// Full D.4 pipeline: detect harnesses -> run each (300s default) ->
  /// gate (all-zero -> pass + verify.json ships with patch; any non-zero
  /// -> stays active + verify-failed + I6 retry budget). Docs-only /
  /// harness-absent trees need a BRAIN waiver line, never worker waiver.
  virtual VerifyReport verify(const std::string& worktreePath,
                              const std::string& taskId, bool docsOnly,
                              bool brainWaived) = 0;

  /// Backend this host will use (lets callers log degradation up front).
  virtual SandboxBackend backend() const = 0;
};

// ---------------------------------------------------------------------------
// §11 TaskArena — DAG + splitOnce gate order + ledger (spec §A.5/A.6/A.8)
// ---------------------------------------------------------------------------

enum class TaskState : std::uint8_t {
  Queued = 0,
  Active = 1,
  Proposed = 2,  ///< patch + verify.json + vector durably written
  Merged = 3,
  Discarded = 4,
  SpiralStopped = 5,  ///< SPIRAL: decomposition bug, awaiting instructions
};

struct TaskNode {
  std::string id;  ///< depth = id.split('.').length - 1 (root 0)
  std::string parentId;
  std::string criterion;
  std::string workerModel;  ///< hetero rule: multi-child splits need >= 2
  int depth = 0;
  int resplitCount = 0;  ///< 0 at enqueue, +1 on parent per splitOnce
  int verifyFailures = 0;
  int retries = 0;
  bool atomic = false;  ///< I8: atomic tasks reject children
  bool alreadySplit = false;  ///< I3: one delegation step per parent id
  TaskState state = TaskState::Queued;
};

/// Ledger event types (existing, preserved): split | report | verify,
/// plus middleware DENY + BUDGET_WARN lines (spec §A.8).
enum class LedgerEvent : std::uint8_t {
  Split = 0,
  Report = 1,
  Verify = 2,
  Deny = 3,
  BudgetWarn = 4,
};

struct LedgerLine {
  LedgerEvent event = LedgerEvent::Report;
  std::string taskId;
  std::string parentId;
  int depth = 0;
  std::string model;
  std::string role;
  std::uint64_t timestampMs = 0;
  std::string criterion;
  std::string verdict;
  std::string detail;
  int resplitCount = 0;
};

/// Owns the task DAG, enforces splitOnce() gate order
/// (spiral BEFORE budget — spec §A.5 normative), and appends every
/// decision to the JSONL ledger.
class TaskArena {
 public:
  virtual ~TaskArena() = default;

  virtual GateCode enqueue(TaskNode task) = 0;

  /// One delegation step. Children inherit the parent's NEW resplit_count
  /// (loop.ts:634-646). Lineage at resplit_count >= 3 fails with
  /// SPIRAL + report line + SpiralStop — even if budget would also deny.
  virtual std::pair<GateCode, std::vector<TaskNode>> splitOnce(
      const std::string& parentId, const std::vector<std::string>& criteria,
      const std::vector<std::string>& workerModels) = 0;

  /// Terminal report: enforces GATE-TURN (I7/I5/I6/I9/I10) then runs the
  /// E.2 compaction pipeline (prune -> vector -> release-after-artifacts
  /// -> L2 post + DAG update + nudge trigger).
  virtual GateCode report(const ResultVector& vector,
                          const std::string& checkArtifact) = 0;

  virtual void appendLedger(const LedgerLine& line) = 0;
  virtual std::optional<TaskNode> find(const std::string& id) const = 0;
  virtual std::vector<TaskNode> childrenOf(
      const std::string& parentId) const = 0;
};

// ---------------------------------------------------------------------------
// §12 Factory helpers (implemented in middleware.cpp / sandbox.cpp)
// ---------------------------------------------------------------------------

class InProcessSisterLeafBus;  // test/fallback impl; defined in middleware.cpp

/// Create the default host implementations for this platform.
/// shmPath: POSIX shm name for the sister bus (e.g. "/brain-sisters");
/// empty => in-process bus (tests, or macOS shm fallback).
std::unique_ptr<SisterLeafBus> makeSisterLeafBus(const std::string& shmPath);
std::unique_ptr<TokenFirewall> makeTokenFirewall(TokenFirewallConfig cfg);
std::unique_ptr<ForwardNudgeLoop> makeForwardNudgeLoop(NudgeConfig cfg);
std::unique_ptr<WorktreeManager> makeWorktreeManager(
    const std::string& repoRoot);
std::unique_ptr<SandboxRunner> makeSandboxRunner();
std::unique_ptr<TaskArena> makeTaskArena(TokenFirewall& firewall,
                                         SisterLeafBus& bus,
                                         const std::string& ledgerPath);

}  // namespace harness
