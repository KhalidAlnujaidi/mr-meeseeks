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

#include <nlohmann/json.hpp>

#include "dshlite/compaction.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/llm_client.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/payload_gate.hpp"
#include "dshlite/postcondition.hpp"

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

  /// Host-plane config (Gap 2 integration): compaction budget, spawn
  /// payload policy, and an OPTIONAL non-owning ledger (F32: null =
  /// no emission; existing callers keep working unchanged).
  struct HostConfig {
    CompactionConfig compaction;
    PolicyConfig policy;
    LedgerWriter* ledger = nullptr;  ///< non-owning; caller outlives loop
  };
  void setHostConfig(HostConfig cfg) { host_ = std::move(cfg); }
  const HostConfig& hostConfig() const { return host_; }

  /// Per-task-lineage nudge/retry state (G2.2): the host owns the map,
  /// one instance per task id, surviving turns until the lineage stops.
  NudgeState& nudgeState(const std::string& taskId);

  /// What one gated task attempt sequence ended in.
  struct TaskReport {
    std::string taskId;
    /// "verified" | "verified-exit-only" | "verify-failed-stop" |
    /// "gate-refused" | "propose-only" | "stop-report" | "spawn-error"
    ///
    /// F72 promotion: "verified" means exit0 AND a content postcondition
    /// held. "verified-exit-only" means exit0 with NO postcondition
    /// declared — honest about being layer 1 only, never a silent
    /// upgrade (F72 wording).
    std::string disposition;
    VerifyOutcome verify;
    GateVerdict gate;
    int spawns = 0;  ///< actual subprocess executions (0 on every refuse)
    /// F72 layer 2 outcome. evaluated=false => no postcondition declared.
    PostconditionResult postcondition;
  };

  /// F72 promotion: host-declared artifact postcondition for a task.
  /// Called with the spawn workspace dir AFTER layer 1 passes and BEFORE
  /// the workspace is cleaned. Returns the layer-2 verdict. The host owns
  /// the ground truth; the model never supplies it (sycophancy-immune).
  using PostconditionHook =
      std::function<PostconditionResult(const std::string& workspaceDir)>;

  /// Decides the NEXT attempt's scope after a verification failure.
  /// round = failures so far (0-based); may rewrite `payload` (narrower
  /// args). F20: the state machine cannot see scope — the host declares
  /// it, and a false FullScope declaration past the cap is refused by
  /// recordRetry itself. Default planner: always FullScope (=> the I6
  /// cap stops the lineage after 2 retries).
  using RetryPlanner =
      std::function<RetryScope(int round, nlohmann::json& payload)>;

  /// G2.4 host loop: gate -> spawn -> verify -> retry/nudge caps.
  /// Sequencing law (payload_gate.hpp call-site contract): NO payload
  /// reaches SwarmSpawner::spawn unchecked; DESTRUCTIVE_PROPOSE_ONLY
  /// never spawns; every refusal/verify/nudge is a ledger line when a
  /// ledger is configured. F31: allowedEnv stays caller-owned — this
  /// loop never adds keys to SpawnOptions.
  ///
  /// F72 promotion: when `postcondition` is supplied, layer 1 (exit
  /// code) must pass AND layer 2 (host-declared artifact postcondition,
  /// evaluated in the workspace BEFORE cleanup) must hold for
  /// disposition "verified". With no hook, the disposition is
  /// "verified-exit-only" — honest about being layer 1 only, never a
  /// silent upgrade. Existing callers pass no hook and keep their
  /// behavior with the honest label.
  TaskReport runGatedTask(const std::string& taskId,
                          const nlohmann::json& payload,
                          const SpawnOptions& opt,
                          RetryPlanner planner = {},
                          PostconditionHook postcondition = {});

  /// G2.4 solicit: ask the model for a tool payload under a
  /// response_format grammar (built from `tools`), then STRICT-parse
  /// the reply (F45: the grammar is a draft accelerator, never an
  /// output guarantee — prose/malformed replies are possible on ANY
  /// engine). On success the caller holds pre-validated JSON ready for
  /// runGatedTask's checkPayload — the gate receives structure, not
  /// prose, which is what reduces nudge loops. On format failure the
  /// caller owns retry/nudge (this method never loops, never repairs,
  /// never extracts substrings). History is NOT touched: solicitation
  /// is a leaf question, not an executive turn.
  struct SolicitResult {
    bool ok = false;              ///< strict parse succeeded
    nlohmann::json payload;       ///< parsed {"tool":...,"args":...} when ok
    std::string raw;              ///< verbatim model content (evidence)
    std::string formatError;      ///< PayloadFormatError text when !ok
  };
  SolicitResult solicitToolPayload(const std::string& prompt,
                                   const std::vector<ToolSchema>& tools);

  /// One executive turn: strategy text in, assistant text out.
  /// Appends to history_; throws only what ILlmPoster::post throws.
  /// Integration laws: the user message is appended BEFORE compaction
  /// (F28 — compaction protects the LAST user message, which is this
  /// input), and every lineage gets recordUserTurn (F29: nudgeDepth
  /// resets; retry rounds are NEVER laundered by a user turn).
  std::string turn(const std::string& userInput);

  /// Judge-gated delegation. Returns the verdict; the CALLER may spawn
  /// a worker only on DoDirect / SplitOnce / StrictVerify. Escalate and
  /// SpiralStop carry a human-readable message in verdict.detail.
  JudgeVerdict gateDelegation(const std::string& criterion);

  const std::vector<Message>& history() const { return history_; }

 private:
  void emit(const LedgerEvent& ev);  // no-op when host_.ledger == null

  ILlmPoster& llm_;
  JudgeHook judge_;
  std::vector<Message> history_;
  HostConfig host_;
  std::map<std::string, NudgeState> nudgeStates_;
};

}  // namespace dshlite
