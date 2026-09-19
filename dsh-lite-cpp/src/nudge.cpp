// nudge.cpp — Gap 2 (G2.2/G2.3/G2.6) pure state machine + local
// verification helpers. See include/dshlite/nudge.hpp for the contract.
// Caps ported verbatim: harness/loop.ts (MAX_ROUNDS_PER_TASK=2) +
// skills/forward-nudge/GUARDRAILS.md (MAX_NUDGE_DEPTH=3, user turn
// resets depth, 3rd retry must be narrower or a different approach).

#include "dshlite/nudge.hpp"

#include <cstdlib>
#include <stdexcept>

#include "dshlite/sanitizer.hpp"

namespace dshlite {

bool nudgeAllowed(const NudgeState& s) {
  // G2.2: the 4th chained auto-continuation is refused BY CONSTRUCTION.
  return !s.stopped && s.nudgeDepth < kMaxNudgeDepth;
}

void recordNudge(NudgeState& s, const std::string& reason) {
  if (!nudgeAllowed(s)) {
    // Fail loud: name the cap, the number, and the recovery.
    throw std::logic_error(
        "nudge cap: task " + (s.taskId.empty() ? std::string("<unset>") : s.taskId) +
        " at nudgeDepth " + std::to_string(s.nudgeDepth) + "/" +
        std::to_string(kMaxNudgeDepth) + " (stopped=" + (s.stopped ? "true" : "false") +
        ") — maxNudgeDepth=3 (I7) exhausted; the 4th chained auto-continuation "
        "is rejected by construction (G2.2). Recovery: an explicit user turn "
        "(recordUserTurn) resets nudgeDepth, or stop and report");
  }
  ++s.nudgeDepth;
  s.lastReason = reason;
}

void recordUserTurn(NudgeState& s) {
  // I7: a real user turn resets chained-continuation depth. It does NOT
  // reset `rounds` (per-task verification budget, I6) and does NOT clear
  // `stopped` — a stopped lineage stays stopped until re-tasked (F27).
  s.nudgeDepth = 0;
}

RetryVerdict recordRetry(NudgeState& s, RetryScope scope) {
  if (s.stopped) return RetryVerdict::StopAndReport;
  if (s.rounds < kMaxRoundsPerTask) {
    ++s.rounds;  // retry 1 or 2 of MAX_ROUNDS_PER_TASK=2 (I6)
    return RetryVerdict::Allow;
  }
  if (s.rounds == kMaxRoundsPerTask) {
    if (scope == RetryScope::NarrowOrReroute) {
      // 3rd attempt allowed ONLY narrower or re-routed to a different
      // model family (GUARDRAILS.md MAX_ROUNDS_PER_TASK rationale +
      // doctrine rule 5: never silently redo full-scope labor).
      ++s.rounds;
      return RetryVerdict::Allow;
    }
    // 3rd FULL-scope retry: rejected by construction (G2.2).
    return RetryVerdict::MustNarrowOrReroute;
  }
  // rounds > 2 means the narrowed 3rd attempt also failed: terminal.
  s.stopped = true;
  return RetryVerdict::StopAndReport;
}

bool isRepeatLoop(std::string_view text, int k) {
  // G2.3: same non-empty line >= k times CONSECUTIVELY = degenerate
  // repeat loop (stall variant, reason "repeat-loop"), never success.
  if (k <= 1) k = 2;
  size_t pos = 0;
  int run = 0;
  std::string_view prev;
  bool havePrev = false;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view line = text.substr(
        pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
    // Trim trailing CR (CRLF streams) and skip empty lines entirely —
    // blank-line runs are formatting, not degeneration.
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.remove_suffix(1);
    if (!line.empty()) {
      if (havePrev && line == prev) {
        ++run;
      } else {
        run = 1;
        prev = line;
        havePrev = true;
      }
      if (run >= k) return true;
    }
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return false;
}

VerifyOutcome verifyViaSpawn(const SpawnResult& r) {
  // G2.6/F9: local check = exit code of the spawned verification command
  // inside the task workspace. No judge hook, no remote API, no TS lane.
  VerifyOutcome v;
  v.exitCode = r.exitCode;
  v.timedOut = r.timedOut;
  v.pass = !r.timedOut && r.exitCode == 0;
  v.detail = sanitize(v.pass ? std::string_view("verify: exit 0")
                             : std::string_view(r.sanitizedStderr.empty()
                                                    ? r.sanitizedStdout
                                                    : r.sanitizedStderr));
  return v;
}

void assertNoRemoteJudgeEnv() {
  // G2.6 startup law (F9 + Ownership decision): this lane verifies
  // locally. A remote-judge key in the harness environment means the
  // zero-network guarantee is broken — fail loud at startup, not later.
  for (const char* name : {"TYPESAFE_API_KEY", "OPENROUTER_API_KEY"}) {
    if (const char* v = std::getenv(name); v != nullptr && *v != '\0')
      throw std::runtime_error(
          std::string("nudge/G2.6: ") + name +
          " is present in the harness environment — the Colibri-native lane "
          "runs local C++ verification only (zero network, F9). Recovery: "
          "unset " + name + " before starting the harness");
  }
}

}  // namespace dshlite
