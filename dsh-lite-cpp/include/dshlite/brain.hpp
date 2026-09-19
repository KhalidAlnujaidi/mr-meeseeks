#pragma once
// brain.hpp — Module 3b: Executive Iteration Loop (SRS Milestone 3).
//
// The Brain keeps strategy + verification state as std::vector<Message>.
// It NEVER executes labor: delegation goes through gateDelegation(), which
// runs the Atomicity Judge hook FIRST. Judge timeout / transport failure
// escalates to the user — never unverified execution, never a lockup.

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dshlite/llm_client.hpp"

namespace dshlite {

enum class JudgeAction {
  DoDirect,      ///< atomic: act now, strict-verify the return
  SplitOnce,     ///< non-atomic: split ONCE into smallest useful sub-tasks
  StrictVerify,  ///< mid confidence: do_direct + strict verification
  Escalate,      ///< low confidence or judge unavailable: ask the user
  SpiralStop,    ///< re-split risk: stop lineage, report, await instructions
};

struct JudgeVerdict {
  JudgeAction action = JudgeAction::Escalate;
  std::string route;  ///< "do_direct" | "split_once" (empty when fallback)
  double confidence = 0.0;
  bool fallback = false;  ///< true => judge unavailable, MUST escalate
  std::string detail;     ///< sanitized judge output / error text
};

/// Routing hook invoked BEFORE any delegation. Throwing / timing out
/// inside the hook is treated as judge-unavailable (fallback escalate).
using JudgeHook = std::function<JudgeVerdict(const std::string& criterion)>;

/// Default hook: runs `node <loopTs> judge <criterion>` inside an isolated
/// worker (Module 2: scrubbed env, pinned CWD, timeout, sanitized output)
/// and parses the `route=... conf=...` / `action=...` lines.
/// Pass the judge key via allowedEnv ONLY when live verdicts are wanted;
/// otherwise the judge self-reports fallback and the Brain escalates.
JudgeHook makeNodeJudgeHook(
    const std::string& loopTs = "harness/loop.ts",
    const std::map<std::string, std::string>& allowedEnv = {},
    std::chrono::milliseconds timeout = std::chrono::seconds(35));

class BrainLoop {
 public:
  BrainLoop(ILlmPoster& llm, JudgeHook judge);

  void setSystemPrompt(const std::string& s);
  /// One executive turn: strategy text in, assistant text out.
  /// Appends to history_; throws only what ILlmPoster::post throws.
  std::string turn(const std::string& userInput);

  /// Judge-gated delegation. Returns the verdict; the CALLER may spawn
  /// a worker only on DoDirect / SplitOnce / StrictVerify. Escalate and
  /// SpiralStop carry a human-readable message in verdict.detail.
  JudgeVerdict gateDelegation(const std::string& criterion);

  const std::vector<Message>& history() const { return history_; }

 private:
  ILlmPoster& llm_;
  JudgeHook judge_;
  std::vector<Message> history_;
};

}  // namespace dshlite
