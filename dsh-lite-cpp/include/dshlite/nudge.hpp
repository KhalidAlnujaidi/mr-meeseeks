#pragma once
// nudge.hpp — Gap 2 (G2.2/G2.3/G2.6): Host-enforced nudge-depth and
// retry-cap state machine + repeat-loop stall variant + local
// verification helpers. Pure state: NO I/O, no sockets, trivially
// unit-testable (roadmap docs/colibri-roadmap.md Gap 2).
//
// Caps ported VERBATIM from the TS reference lane:
//   harness/loop.ts + skills/forward-nudge/GUARDRAILS.md
//   - MAX_NUDGE_DEPTH = 3 chained auto-continuations without a user
//     turn (I7). A user turn resets nudgeDepth to 0; it NEVER resets
//     per-task retry rounds (those are per task id, I6).
//   - MAX_ROUNDS_PER_TASK = 2 verification-failure retries per task id
//     (I6). The 3rd attempt must be NARROWER or RE-ROUTED to a
//     different model family; a 3rd full-scope retry is rejected by
//     construction (MustNarrowOrReroute), and anything past the
//     narrowed 3rd attempt is StopAndReport — never silently redo
//     full-scope labor (meeseeks-doctrine rule 5).
//
// The 4th nudge and the 3rd full-scope retry MUST be rejected here —
// prompt text contradicting these gates loses (standing doctrine 5).

#include <string>
#include <string_view>

#include "dshlite/spawner.hpp"

namespace dshlite {

/// Ported cap (GUARDRAILS.md "MAX_NUDGE_DEPTH = 3").
inline constexpr int kMaxNudgeDepth = 3;
/// Ported cap (harness/loop.ts MAX_ROUNDS_PER_TASK = 2).
inline constexpr int kMaxRoundsPerTask = 2;
/// G2.3: leaf output stall variant, ledger reason string.
inline constexpr const char* kRepeatLoopReason = "repeat-loop";

enum class RetryVerdict {
  Allow,               ///< retry within the I6 budget
  MustNarrowOrReroute, ///< rounds == 2: a 3rd FULL-scope retry is refused;
                       ///< narrow the task or re-route to a different model
                       ///< family (Gap 1 fallback), else stop+report
  StopAndReport,       ///< cap exhausted: plain report, no auto-continue
};

/// What the host promises about the next attempt's scope. The state
/// machine cannot see scope itself — the host declares it, and a false
/// declaration is a doctrine violation, not something this pure machine
/// can police (F20: the plan's recordRetry(state) alone cannot know).
enum class RetryScope {
  FullScope,        ///< same task, same family, same scope
  NarrowOrReroute,  ///< narrower task OR different model family
};

/// Per-task-lineage nudge/retry state. One instance per task id; the
/// host owns the map (taskId -> NudgeState).
struct NudgeState {
  std::string taskId;
  int nudgeDepth = 0;  ///< chained auto-continuations since last user turn
  int rounds = 0;      ///< verification-failure retries consumed (I6)
  /// Latched on StopAndReport: the lineage is terminal, every further
  /// nudge/retry is refused until the host starts a NEW task id.
  bool stopped = false;
  std::string lastReason;  ///< reason of the most recent recordNudge
};

/// True while another chained auto-continuation is allowed:
/// nudgeDepth < kMaxNudgeDepth (3) and the lineage is not stopped.
/// The 4th nudge is false BY CONSTRUCTION (G2.2 build-failing test).
bool nudgeAllowed(const NudgeState& s);

/// Record one chained auto-continuation (ledger `nudge` line carries
/// s.nudgeDepth afterwards). Throws std::logic_error when the cap would
/// be exceeded — fail loud, naming the cap, the number, and the
/// recovery (explicit user turn resets nudgeDepth).
void recordNudge(NudgeState& s, const std::string& reason);

/// An explicit user turn happened: nudgeDepth resets to 0 (I7).
/// `rounds` is NOT reset — retry budget is per task id, and a user
/// turn must not launder exhausted verification retries (F27).
void recordUserTurn(NudgeState& s);

/// A verification failure happened and the host wants to retry.
/// Returns the verdict; only on Allow did `rounds` advance.
/// rounds<2 -> Allow; rounds==2 -> MustNarrowOrReroute unless scope is
/// NarrowOrReroute (then Allow, rounds=3); rounds>=3 or stopped ->
/// StopAndReport (latched). Pure, no I/O.
RetryVerdict recordRetry(NudgeState& s, RetryScope scope);

/// G2.3 repetitive-output detection: true when any non-empty line
/// repeats >= k times CONSECUTIVELY (degenerate leaf loop) — treated as
/// a stall variant (ledger reason "repeat-loop"), never as success.
bool isRepeatLoop(std::string_view text, int k = 8);

/// G2.6 local verification gate (F9): the C++ lane judges by the exit
/// code of the spawned verification command inside the task workspace —
/// sycophancy-immune (spec D.4), no remote judge, no TS sidecar.
struct VerifyOutcome {
  bool pass = false;
  int exitCode = 127;
  bool timedOut = false;
  std::string detail;  ///< sanitized (Module 1), <= 4096 chars
};
VerifyOutcome verifyViaSpawn(const SpawnResult& r);

/// G2.6 startup law: throws std::runtime_error when a remote-judge key
/// is present in the harness environment (TYPESAFE_API_KEY or
/// OPENROUTER_API_KEY) — this lane verifies locally, zero network (F9,
/// roadmap Ownership decision). Loopback COLI keys are NOT touched.
void assertNoRemoteJudgeEnv();

}  // namespace dshlite
