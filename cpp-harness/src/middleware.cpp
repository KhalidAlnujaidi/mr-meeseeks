// middleware.cpp — host-enforced middleware: gates, bus, nudge loop, arena.
//
// Implements the contracts from include/harness.hpp:
//   §2  gateMessage() normative wording (spec §A.7)
//   §5  InProcessSisterLeafBus (tests / macOS shm fallback)
//   §6  ResultVector validate/statusString/parseStatus (spec §E.2.3)
//   §7  TokenFirewall B^D pre-call proxy (spec §A.3 / §D.1)
//   §8  ForwardNudgeLoop stall/yield loop, depth 3 (spec §D.2)
//   §11 TaskArena DAG + splitOnce (spiral BEFORE budget) + ledger (spec §A.5/6/8)
//
// Portable C++20, STL + POSIX clocks only. No third-party dependencies.

#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace harness {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::uint64_t nowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

/// Append one JSON object line, creating parent dirs. Fail-closed: a ledger
/// write failure never throws into gate logic (best-effort audit).
void appendJsonl(const std::string& path, const std::string& obj) {
  if (path.empty()) return;
  try {
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream f(path, std::ios::app);
    if (f) f << obj << "\n";
  } catch (...) {
    // Audit-only; gates must not fail because the ledger is unavailable.
  }
}

bool containsDestructiveVerb(const std::string& criterion) {
  static const char* kVerbs[] = {"push",   "publish",  "delete",  "migrate",
                                 "credential", "rm -rf", "drop table", "secret"};
  std::string lower = criterion;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const char* v : kVerbs) {
    if (lower.find(v) != std::string::npos) return true;
  }
  return false;
}

std::string ledgerEventName(LedgerEvent e) {
  switch (e) {
    case LedgerEvent::Split: return "split";
    case LedgerEvent::Report: return "report";
    case LedgerEvent::Verify: return "verify";
    case LedgerEvent::Deny: return "deny";
    case LedgerEvent::BudgetWarn: return "budget_warn";
  }
  return "report";
}

}  // namespace

// ---------------------------------------------------------------------------
// §2 gateMessage — exact brain-visible wording per spec §A.7
// ---------------------------------------------------------------------------

std::string gateMessage(GateCode code, const std::string& taskId,
                        long estimate, long budget) {
  std::ostringstream o;
  switch (code) {
    case GateCode::Ok: return "ok";
    case GateCode::BudgetWarn:
      o << "warning: estimate " << estimate << " is "
        << (budget > 0 ? (100 * estimate / budget) : 0)
        << "% of tree budget " << budget << " (task " << taskId
        << ") — proceeding, prune follow-ups";
      return o.str();
    case GateCode::SpiralStop:
      return "task " + taskId +
             ": resplit_count reached 3, lineage stopped — SPIRAL: "
             "decomposition bug, awaiting instructions";
    case GateCode::BudgetExceeded:
      o << "task " << taskId << ": split refused, breadth^depth estimate "
        << estimate << " > budget " << budget;
      return o.str();
    case GateCode::BreadthCap:
      o << "task " << taskId << ": split refused, breadth " << estimate
        << " > per-depth cap " << budget;
      return o.str();
    case GateCode::DailyCap:
      o << "day budget exhausted: used " << budget << "/"
        << DAILY_REQUEST_CAP << " requests, proposed +" << estimate
        << " — stopped, resume tomorrow or narrow scope";
      return o.str();
    case GateCode::DepthFirebreak:
      o << "spawn refused: depth " << estimate << " > memberMaxDepth "
        << budget << " (task " << taskId << ")";
      return o.str();
    case GateCode::HomogeneousAssignment:
      o << "task " << taskId
        << ": multi-child split needs >=2 distinct workerModel (got: "
        << estimate << " distinct)";
      return o.str();
    case GateCode::DestructiveProposeOnly:
      return "proposed only, not enqueued (destructive): " + taskId +
             " — needs explicit user approval";
    case GateCode::MaxRounds:
      return "task " + taskId +
             ": failed verification twice — no full-scope retry; propose "
             "narrower attempt or escalate";
    case GateCode::AtomicLocked:
      return "task " + taskId + ": atomic task rejects children (I8)";
    case GateCode::SplitOnce:
      return "task " + taskId +
             ": already split once (I3) — one delegation step per parent id";
    case GateCode::BusValueTooLarge:
      return "task " + taskId +
             ": bus value exceeds 64 KiB cap — entry unchanged";
    case GateCode::ArtifactsPending:
      return "task " + taskId +
             ": release blocked — artifacts pending (patch/verify.json/vector)";
    case GateCode::VerifyFail:
      return "task " + taskId +
             ": verification failed (V_VERIFY_FAIL) — patch withheld, worker "
             "retries";
    case GateCode::VerifyTimeout:
      return "task " + taskId + ": verification timed out (V_VERIFY_TIMEOUT)";
    case GateCode::HarnessAbsent:
      return "task " + taskId +
             ": no verify harness — waiver path, never silent-pass "
             "(V_HARNESS_ABSENT)";
    case GateCode::WorktreeMainDirty:
      return "task " + taskId +
             ": main worktree dirty (WT_MAIN_DIRTY) — commit or stash first";
    case GateCode::WorktreeStaleBase:
      return "task " + taskId +
             ": stale base branch (WT_STALE_BASE) — rebase or reacquire";
    case GateCode::WorktreeLocked:
      return "task " + taskId + ": worktree locked or broken (WT_LOCKED)";
    case GateCode::WorktreeBranchCheckedOut:
      return "task " + taskId +
             ": branch already checked out (WT_BRANCH_CHECKED_OUT)";
    case GateCode::WorktreePathExists:
      return "task " + taskId + ": worktree path exists (WT_PATH_EXISTS)";
    case GateCode::WorktreeMergeConflict:
      return "task " + taskId + ": merge conflict (WT_MERGE_CONFLICT)";
    case GateCode::WorktreeUntrackedOnly:
      return "task " + taskId + ": untracked-only diff (WT_UNTRACKED_ONLY)";
    case GateCode::WorktreeGitignoreMissing:
      return "task " + taskId +
             ": flat layout needs .worktrees/ in .gitignore "
             "(WT_GITIGNORE_MISSING)";
  }
  return "task " + taskId + ": unknown gate code";
}

// ---------------------------------------------------------------------------
// §6 ResultVector
// ---------------------------------------------------------------------------

GateCode ResultVector::validate(bool patchExists) const {
  if (taskId.empty()) return GateCode::VerifyFail;
  // 1–5 summary lines.
  std::size_t lines = 1;
  for (char c : summary) {
    if (c == '\n') ++lines;
  }
  if (summary.empty() || lines > 5) return GateCode::VerifyFail;
  // Patch binding: REQUIRED iff the patch exists (spec §E.2.3).
  if (patchExists && gitDiffHash.empty()) return GateCode::VerifyFail;
  // Exported state: scalar strings, <= 4 KiB serialized (never truncate).
  std::size_t bytes = 0;
  for (const auto& [k, v] : exportedState) {
    bytes += k.size() + v.size() + 8;  // JSON overhead per entry
    for (char c : v) {
      if (c == '\n' || c == '\0') return GateCode::BusValueTooLarge;
    }
    if (bytes > EXPORTED_STATE_MAX_BYTES) return GateCode::BusValueTooLarge;
  }
  return GateCode::Ok;
}

std::string ResultVector::statusString() const {
  switch (status) {
    case TaskStatus::Success: return "SUCCESS";
    case TaskStatus::Failed: return "FAILED";
    case TaskStatus::Discarded: return "DISCARDED";
  }
  return "FAILED";
}

std::optional<TaskStatus> ResultVector::parseStatus(const std::string& s) {
  std::string lower = s;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower == "success") return TaskStatus::Success;
  if (lower == "failed") return TaskStatus::Failed;
  if (lower == "discarded") return TaskStatus::Discarded;
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// §5 InProcessSisterLeafBus — mutex-guarded map, partition-scoped reads
// ---------------------------------------------------------------------------

class InProcessSisterLeafBus : public SisterLeafBus {
 public:
  GateCode publish(const SisterLeafEntry& entry) override {
    if (entry.value.size() > BUS_VALUE_MAX_BYTES) {
      return GateCode::BusValueTooLarge;  // entry unchanged
    }
    SisterLeafEntry stamped = entry;
    stamped.timestampMs = nowMs();
    if (stamped.updatedBy.empty()) stamped.updatedBy = "host";
    std::lock_guard<std::mutex> lock(mu_);
    map_[Key(entry.parentSessionId, entry.namespace_, entry.key)] = stamped;
    return GateCode::Ok;
  }

  std::optional<SisterLeafEntry> read(const std::string& parentSessionId,
                                      const std::string& namespace_,
                                      const std::string& key) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(Key(parentSessionId, namespace_, key));
    if (it == map_.end()) return std::nullopt;  // fail-closed, no cross-partition
    return it->second;
  }

  std::map<std::string, SisterLeafEntry> listNamespace(
      const std::string& parentSessionId, const std::string& namespace_) override {
    std::map<std::string, SisterLeafEntry> out;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [k, v] : map_) {
      if (v.parentSessionId == parentSessionId && v.namespace_ == namespace_) {
        out.emplace(k, v);
      }
    }
    return out;
  }

 private:
  static std::string Key(const std::string& p, const std::string& n,
                         const std::string& k) {
    std::string key;
    key.reserve(p.size() + n.size() + k.size() + 2);
    key += p;
    key.push_back('\0');
    key += n;
    key.push_back('\0');
    key += k;
    return key;
  }

  std::mutex mu_;
  std::unordered_map<std::string, SisterLeafEntry> map_;
};

// ---------------------------------------------------------------------------
// §7 TokenFirewall — normative GATE-SPAWN / GATE-SPLIT order (spec §A.6.2)
// ---------------------------------------------------------------------------

// TokenFirewall base holds no state (impl owns the config); the ctor exists
// to satisfy the header's explicit declaration.
TokenFirewall::TokenFirewall(TokenFirewallConfig) {}

class TokenFirewallImpl : public TokenFirewall {
 public:
  explicit TokenFirewallImpl(TokenFirewallConfig cfg)
      : TokenFirewall(cfg), cfg_(std::move(cfg)), dayUsed_(0) {}

  FirewallDecision checkSpawn(const std::string& taskId, int childDepth,
                              long dayUsed) override {
    // (a) I1 member-depth firebreak first: bounds worst-case cost.
    if (childDepth > MEMBER_MAX_DEPTH) {
      return Deny(taskId, GateCode::DepthFirebreak, /*estimate=*/childDepth,
                  /*budget=*/MEMBER_MAX_DEPTH, dayUsed);
    }
    // (b) Shared pre-call daily cap (breadth = 1 for a single spawn).
    if (dayUsed + 1 > cfg_.dailyRequestCap) {
      return Deny(taskId, GateCode::DailyCap, /*estimate=*/1, dayUsed, dayUsed);
    }
    // (c) Destructive-verb scan: no criterion text exists at spawn time, so
    // screening applies where criteria exist (checkSplit / report). A single
    // spawn is one request; allow it and account for it.
    FirewallDecision d;
    d.verdict = GateVerdict::Allow;
    d.code = GateCode::Ok;
    d.estimate = 1;
    d.dayUsed = dayUsed;
    d.message = gateMessage(GateCode::Ok, taskId);
    return d;
  }

  FirewallDecision checkSplit(
      const std::string& taskId, int parentDepth, int breadth,
      int resplitCount, bool parentAtomic, bool parentAlreadySplit,
      const std::vector<std::string>& workerModels, long /*treeUsed*/,
      long dayUsed) override {
    // (a) Spiral breaker BEFORE budget (spec §A.5 normative order): even if
    // the budget would also deny, the lineage failure must read SPIRAL.
    if (resplitCount >= MAX_RESPLITS) {
      return Deny(taskId, GateCode::SpiralStop, /*estimate=*/resplitCount,
                  /*budget=*/MAX_RESPLITS, dayUsed);
    }
    // (b) Hetero-assignment (R2-B): multi-child splits need >= 2 models.
    if (breadth > 1) {
      std::set<std::string> distinct(workerModels.begin(), workerModels.end());
      if (distinct.size() < 2) {
        return Deny(taskId, GateCode::HomogeneousAssignment,
                    /*estimate=*/static_cast<long>(distinct.size()),
                    /*budget=*/2, dayUsed);
      }
    }
    // (c) Per-depth breadth cap regardless of estimate.
    if (breadth > cfg_.perDepthBreadthCap) {
      return Deny(taskId, GateCode::BreadthCap, /*estimate=*/breadth,
                  /*budget=*/cfg_.perDepthBreadthCap, dayUsed);
    }
    // (d) B^D vs per-tree budget: warn in (70%, 100%], deny above 100%.
    // H1: __int128 accumulation — exact for in-range values (5^3 = 125),
    // saturates to budget+1 ONLY past LONG_MAX. No `long` overflow, no UB.
    long exact =
        estimatePreCallChecked(breadth, parentDepth, cfg_.perTreeRequestBudget);
    if (exact > cfg_.perTreeRequestBudget) {
      return Deny(taskId, GateCode::BudgetExceeded, exact,
                  cfg_.perTreeRequestBudget, dayUsed);
    }
    long estimate = exact;
    // Destructive verbs are never auto-executed: propose-only.
    // (Checked here where criteria-bearing splits exist.)
    // NOTE: criteria text is not passed to checkSplit; the arena performs
    // the verb scan per criterion in splitOnce() and maps hits to
    // ProposeOnly before enqueueing. This hook keeps the firewall honest
    // about what it screens: breadth/depth/budget/daily.
    // (e) Shared daily cap.
    if (dayUsed + estimate > cfg_.dailyRequestCap) {
      return Deny(taskId, GateCode::DailyCap, estimate, dayUsed, dayUsed);
    }
    // (f) Split-once (I3) + atomic lock (I8).
    if (parentAlreadySplit) {
      return Deny(taskId, GateCode::SplitOnce, estimate,
                  cfg_.perTreeRequestBudget, dayUsed);
    }
    if (parentAtomic) {
      return Deny(taskId, GateCode::AtomicLocked, estimate,
                  cfg_.perTreeRequestBudget, dayUsed);
    }
    FirewallDecision d;
    const long warnAt = static_cast<long>(cfg_.softWarnRatio *
                                          cfg_.perTreeRequestBudget);
    if (estimate > warnAt) {
      d.verdict = GateVerdict::Warn;
      d.code = GateCode::BudgetWarn;
      d.message = gateMessage(GateCode::BudgetWarn, taskId, estimate,
                              cfg_.perTreeRequestBudget);
      appendJsonl(cfg_.ledgerPath,
                  "{\"event\":\"budget_warn\",\"task_id\":\"" +
                      jsonEscape(taskId) + "\",\"estimate\":" +
                      std::to_string(estimate) + ",\"budget\":" +
                      std::to_string(cfg_.perTreeRequestBudget) + ",\"ts\":" +
                      std::to_string(nowMs()) + "}");
    } else {
      d.verdict = GateVerdict::Allow;
      d.code = GateCode::Ok;
      d.message = gateMessage(GateCode::Ok, taskId);
    }
    d.estimate = estimate;
    d.dayUsed = dayUsed;
    return d;
  }

  void recordRequest(const std::string& taskId) override {
    long used = ++dayUsed_;
    appendJsonl(cfg_.ledgerPath,
                "{\"event\":\"request\",\"task_id\":\"" + jsonEscape(taskId) +
                    "\",\"day_used\":" + std::to_string(used) + ",\"ts\":" +
                    std::to_string(nowMs()) + "}");
  }

  long dayUsed() const override { return dayUsed_.load(); }

 private:
  FirewallDecision Deny(const std::string& taskId, GateCode code,
                        long estimate, long budget, long dayUsed) {
    FirewallDecision d;
    d.verdict = GateVerdict::Deny;
    d.code = code;
    d.estimate = estimate;
    d.dayUsed = dayUsed;
    d.message = gateMessage(code, taskId, estimate, budget);
    // Every refusal is auditable: exactly one DENY ledger line, no other
    // state change (fail-closed).
    appendJsonl(cfg_.ledgerPath,
                "{\"event\":\"deny\",\"task_id\":\"" + jsonEscape(taskId) +
                    "\",\"code\":" + std::to_string(static_cast<int>(code)) +
                    ",\"estimate\":" + std::to_string(estimate) +
                    ",\"day_used\":" + std::to_string(dayUsed) + ",\"ts\":" +
                    std::to_string(nowMs()) + "}");
    return d;
  }

  TokenFirewallConfig cfg_;
  std::atomic<long> dayUsed_;
};

// ---------------------------------------------------------------------------
// §8 ForwardNudgeLoop — active stall/yield re-prompt within budget 3
// ---------------------------------------------------------------------------

class ForwardNudgeLoopImpl : public ForwardNudgeLoop {
 public:
  explicit ForwardNudgeLoopImpl(NudgeConfig cfg) : cfg_(cfg) {}

  void heartbeat(const std::string& taskId,
                 std::chrono::steady_clock::time_point now) override {
    std::lock_guard<std::mutex> lock(mu_);
    last_[taskId] = now;
  }

  NudgeAction poll(const std::string& taskId, const NudgeContext& nudge,
                   bool userInterrupted, int consecutiveStalls,
                   int verifyFailures,
                   std::chrono::steady_clock::time_point now) override {
    // User interrupt always wins: cancel + resume-lock.
    if (userInterrupted) return NudgeAction::WithheldInterrupt;
    // I6: verification failed twice — never a 3rd full-scope attempt.
    if (verifyFailures >= cfg_.maxRoundsPerTask) {
      return NudgeAction::EscalateNarrower;
    }
    // I7 shared budget exhausted (3/3): plain report, no re-prompt.
    if (nudge.nudgeDepth >= cfg_.maxNudgeDepth) {
      return NudgeAction::StopAndReport;
    }
    auto last = now;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = last_.find(taskId);
      if (it != last_.end()) last = it->second;
    }
    bool stalled = (now - last) > cfg_.stallAfterMs;
    if (stalled) {
      // 3rd consecutive stall escalates to a different model (or narrower
      // when no fallback model is configured), never a full-scope retry.
      if (consecutiveStalls >= 2) {
        return cfg_.fallbackModel.empty()
                   ? NudgeAction::EscalateNarrower
                   : NudgeAction::EscalateDifferentModel;
      }
      return NudgeAction::Reprompt;
    }
    // Yield-fail with budget left: re-queue SAME (task, attempt) + context.
    if (nudge.reason == NudgeReason::YieldFail) return NudgeAction::Reprompt;
    return NudgeAction::Reprompt;
  }

  std::pair<std::vector<std::string>, std::vector<std::string>>
  pruneParallelPaths(std::vector<std::string> proposed) override {
    std::vector<std::string> keep, deferred;
    for (std::size_t i = 0; i < proposed.size(); ++i) {
      if (static_cast<int>(i) < cfg_.maxParallelPaths) {
        keep.push_back(proposed[i]);
      } else {
        deferred.push_back("Deferred: " + proposed[i]);
      }
    }
    return {keep, deferred};
  }

 private:
  NudgeConfig cfg_;
  std::mutex mu_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_;
};

// ---------------------------------------------------------------------------
// §11 TaskArena — pmr DAG, splitOnce gate order, JSONL ledger
// ---------------------------------------------------------------------------

class TaskArenaImpl : public TaskArena {
 public:
  TaskArenaImpl(TokenFirewall& firewall, SisterLeafBus& bus,
                const std::string& ledgerPath)
      : firewall_(firewall), ledgerPath_(ledgerPath) {
    // Bus is consulted by the report() compaction path via the caller-owned
    // reference below; keep the handle for future E.2 artifact gating.
    (void)bus;
  }

  ~TaskArenaImpl() override = default;

  GateCode enqueue(TaskNode task) override {
    std::lock_guard<std::mutex> lock(mu_);
    // H4: plain std::string keys in a heap unordered_map. The old
    // pmr::monotonic_buffer_resource design leaked one key-sized temp
    // allocation per read (find/childrenOf) because pool_ never freed.
    if (nodes_.find(task.id) != nodes_.end()) return GateCode::Ok;  // idempotent
    task.depth = DepthOf(task.id);
    task.resplitCount = 0;
    task.state = TaskState::Queued;
    nodes_.emplace(task.id, std::move(task));
    return GateCode::Ok;
  }

  std::pair<GateCode, std::vector<TaskNode>> splitOnce(
      const std::string& parentId, const std::vector<std::string>& criteria,
      const std::vector<std::string>& workerModels) override {
    std::lock_guard<std::mutex> lock(mu_);
    auto pit = nodes_.find(parentId);
    if (pit == nodes_.end()) return {GateCode::SplitOnce, {}};  // unknown parent
    TaskNode& parent = pit->second;

    // NORMATIVE ORDER: spiral (I4) BEFORE budget (spec §A.5). A lineage at
    // resplit_count >= 3 fails SPIRAL even if the budget would also deny.
    if (parent.resplitCount >= MAX_RESPLITS) {
      parent.state = TaskState::SpiralStopped;
      LedgerLine line;
      line.event = LedgerEvent::Report;
      line.taskId = parentId;
      line.parentId = parent.parentId;
      line.depth = parent.depth;
      line.verdict = "SPIRAL";
      line.detail = gateMessage(GateCode::SpiralStop, parentId);
      line.resplitCount = parent.resplitCount;
      appendLedger(line);
      return {GateCode::SpiralStop, {}};
    }

    const int breadth = static_cast<int>(criteria.size());
    // Resolve per-child models (cycle when fewer models than criteria), then
    // screen destructive criteria: propose-only, never auto-enqueue.
    std::vector<std::string> childModels;
    childModels.reserve(criteria.size());
    std::vector<std::string> safeCriteria;
    std::vector<std::string> safeModels;
    for (std::size_t i = 0; i < criteria.size(); ++i) {
      std::string model = workerModels.empty()
                              ? std::string("default")
                              : workerModels[i % workerModels.size()];
      childModels.push_back(model);
      if (containsDestructiveVerb(criteria[i])) {
        LedgerLine line;
        line.event = LedgerEvent::Deny;
        line.taskId = parentId + "." + std::to_string(i);
        line.parentId = parentId;
        line.depth = parent.depth + 1;
        line.model = model;
        line.verdict = "DESTRUCTIVE_PROPOSE_ONLY";
        line.detail =
            gateMessage(GateCode::DestructiveProposeOnly, criteria[i]);
        appendLedger(line);
      } else {
        safeCriteria.push_back(criteria[i]);
        safeModels.push_back(model);
      }
    }
    if (safeCriteria.empty()) {
      return {GateCode::DestructiveProposeOnly, {}};
    }

    std::set<std::string> distinct(safeModels.begin(), safeModels.end());
    std::vector<std::string> distinctVec(distinct.begin(), distinct.end());
    FirewallDecision d = firewall_.checkSplit(
        parentId, parent.depth, static_cast<int>(safeCriteria.size()),
        parent.resplitCount, parent.atomic, parent.alreadySplit, distinctVec,
        treeUsed_, firewall_.dayUsed());
    if (d.verdict == GateVerdict::Deny) {
      auto code = d.code;
      if (code == GateCode::BudgetWarn) code = GateCode::Ok;  // warn allows
      if (d.code != GateCode::BudgetWarn) return {d.code, {}};
    }
    if (d.verdict == GateVerdict::ProposeOnly) {
      return {GateCode::DestructiveProposeOnly, {}};
    }
    treeUsed_ += d.estimate;

    // Commit: children inherit the parent's NEW resplit_count (loop.ts:634).
    parent.resplitCount += 1;
    parent.alreadySplit = true;
    parent.state = TaskState::Active;
    std::vector<TaskNode> children;
    for (std::size_t i = 0; i < safeCriteria.size(); ++i) {
      TaskNode child;
      child.id = parentId + "." + std::to_string(i);
      child.parentId = parentId;
      child.criterion = safeCriteria[i];
      child.workerModel = safeModels[i];
      child.depth = parent.depth + 1;
      child.resplitCount = parent.resplitCount;
      child.state = TaskState::Queued;
      children.push_back(child);
      nodes_.emplace(child.id, child);
      children_[parentId].push_back(child.id);
    }
    LedgerLine line;
    line.event = LedgerEvent::Split;
    line.taskId = parentId;
    line.depth = parent.depth;
    line.verdict = d.code == GateCode::BudgetWarn ? "BUDGET_WARN" : "ALLOW";
    line.detail = d.message;
    line.resplitCount = parent.resplitCount;
    appendLedger(line);
    // H5: propagate the firewall's warn code instead of collapsing to Ok,
    // so callers can tell "comfortably within budget" from "prune now".
    // BudgetWarn is a non-error allow (gateMessage says "proceeding").
    (void)breadth;
    return {d.code, children};
  }

  GateCode report(const ResultVector& vector,
                  const std::string& checkArtifact) override {
    // GATE-TURN (I10): close requires a check artifact — file re-read hash,
    // command exit code, or explicit waiver. Empty artifact refuses.
    if (checkArtifact.empty()) return GateCode::VerifyFail;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(vector.taskId);
    if (it == nodes_.end()) return GateCode::SplitOnce;  // unknown task
    TaskNode& node = it->second;
    // I6: two verification failures lock full-scope retry.
    if (node.verifyFailures >= MAX_ROUNDS_PER_TASK) return GateCode::MaxRounds;
    // E.2 compaction: validate the vector (patch binding + 4 KiB state cap).
    // SUCCESS binds a patch hash; FAILED/DISCARDED carry the no-patch reason
    // in the summary instead.
    bool patchExists = (vector.status == TaskStatus::Success);
    GateCode vc = vector.validate(patchExists);
    if (vc != GateCode::Ok) {
      node.verifyFailures += 1;
      return vc;
    }
    node.state = TaskState::Proposed;
    LedgerLine line;
    line.event = LedgerEvent::Report;
    line.taskId = vector.taskId;
    line.parentId = node.parentId;
    line.depth = node.depth;
    line.model = node.workerModel;
    line.verdict = vector.statusString();
    line.detail = checkArtifact;
    line.resplitCount = node.resplitCount;
    appendLedger(line);
    return GateCode::Ok;
  }

  void appendLedger(const LedgerLine& line) override {
    std::ostringstream o;
    o << "{\"event\":\"" << ledgerEventName(line.event) << "\",\"task_id\":\""
      << jsonEscape(line.taskId) << "\",\"parent_id\":\""
      << jsonEscape(line.parentId) << "\",\"depth\":" << line.depth
      << ",\"model\":\"" << jsonEscape(line.model) << "\",\"role\":\""
      << jsonEscape(line.role) << "\",\"ts\":" << nowMs() << ",\"criterion\":\""
      << jsonEscape(line.criterion) << "\",\"verdict\":\""
      << jsonEscape(line.verdict) << "\",\"detail\":\""
      << jsonEscape(line.detail) << "\",\"resplit_count\":" << line.resplitCount
      << "}";
    appendJsonl(ledgerPath_, o.str());
  }

  std::optional<TaskNode> find(const std::string& id) const override {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return std::nullopt;
    return it->second;
  }

  std::vector<TaskNode> childrenOf(const std::string& parentId) const override {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<TaskNode> out;
    auto it = children_.find(parentId);
    if (it == children_.end()) return out;
    for (const auto& cid : it->second) {
      auto nit = nodes_.find(cid);
      if (nit != nodes_.end()) out.push_back(nit->second);
    }
    return out;
  }

 private:
  static int DepthOf(const std::string& id) {
    // depth = id.split('.').length - 1 (root has no dot -> 0).
    int depth = 0;
    for (char c : id) {
      if (c == '.') ++depth;
    }
    return depth;
  }

  TokenFirewall& firewall_;
  std::string ledgerPath_;
  mutable std::mutex mu_;
  // H4: heap unordered_map with std::string keys. Lookup temporaries free
  // on destruction; RSS no longer grows with read volume.
  mutable std::unordered_map<std::string, TaskNode> nodes_;
  mutable std::unordered_map<std::string, std::vector<std::string>> children_;
  long treeUsed_ = 0;  // sum of allowed split estimates in this tree
};

// ---------------------------------------------------------------------------
// §12 Factories
// ---------------------------------------------------------------------------

std::unique_ptr<SisterLeafBus> makeSisterLeafBus(const std::string& shmPath) {
#if HARNESS_OS_LINUX
  // H6: non-empty shmPath => cross-process segment (sister_shm.cpp).
  // Empty => in-process (tests / macOS fallback). Forward-declared here
  // to keep middleware.cpp free of POSIX-shm includes.
  std::unique_ptr<SisterLeafBus> openShmSisterLeafBus(const std::string& name);
  if (!shmPath.empty()) {
    if (auto bus = openShmSisterLeafBus(shmPath)) return bus;
    // Attach/create failure: fall through to in-process rather than
    // returning null (callers never null-check the factory).
  }
#else
  (void)shmPath;
#endif
  return std::make_unique<InProcessSisterLeafBus>();
}

std::unique_ptr<TokenFirewall> makeTokenFirewall(TokenFirewallConfig cfg) {
  return std::make_unique<TokenFirewallImpl>(std::move(cfg));
}

std::unique_ptr<ForwardNudgeLoop> makeForwardNudgeLoop(NudgeConfig cfg) {
  return std::make_unique<ForwardNudgeLoopImpl>(std::move(cfg));
}

std::unique_ptr<TaskArena> makeTaskArena(TokenFirewall& firewall,
                                         SisterLeafBus& bus,
                                         const std::string& ledgerPath) {
  return std::make_unique<TaskArenaImpl>(firewall, bus, ledgerPath);
}

}  // namespace harness
