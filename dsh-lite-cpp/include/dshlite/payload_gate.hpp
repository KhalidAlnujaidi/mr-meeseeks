#pragma once
// payload_gate.hpp — Gap 2 (G2.4/F7): pre-execution payload gate.
//
// Leaves propose tool payloads as structured JSON (grammar-forced where
// the engine supports GRAMMAR=*.gbnf, roadmap G2.4). This gate validates
// schema + policy BEFORE any SwarmSpawner::spawn — no payload reaches a
// subprocess unvalidated.
//
// CALL-SITE CONTRACT (F7): SwarmSpawner stays a DUMB EXECUTOR — the gate
// is NOT wired into spawner.cpp. Every host path that spawns on behalf
// of a leaf MUST sequence:
//
//   1. GateVerdict v = checkPayload(payload, policy);
//   2. if (!v.allowed)              -> refuse; write a DENY ledger line
//                                      {gate:"SPAWN", code:v.code}; never
//                                      spawn. (G2.4: invalid payload =>
//                                      retry within the I6 budget, then
//                                      escalate.)
//   3. if (!autoExecutable(v))      -> PROPOSE-ONLY (destructive verb,
//                                      spec A.3 DESTRUCTIVE_PROPOSE_ONLY):
//                                      record the proposal, await explicit
//                                      user approval; NEVER auto-spawn.
//   4. only when autoExecutable(v)  -> SwarmSpawner::spawn(...).
//
// enforceGateBeforeSpawn() implements steps 2-3 as a fail-loud helper.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dshlite {

/// Host policy for the pre-execution gate.
struct PolicyConfig {
  /// Allowlist of tool names a leaf may propose (exact match). A tool
  /// outside this list is TOOL_NOT_ALLOWED — destructive verbs never
  /// launder an unknown tool into propose-only (ordering, F21).
  std::vector<std::string> allowedTools;
  /// Irreversible-verb list (spec A.3 / GUARDRAILS.md stop condition 5:
  /// push, publish, delete, migrate, credential change => propose only,
  /// never auto-execute). Matched case-insensitively on WORD BOUNDS —
  /// a naive substring scan would demote "information" via "format"
  /// (F21); boundaries keep the scan honest while staying conservative.
  std::vector<std::string> destructiveVerbs = {
      "push", "publish", "delete", "drop", "migrate", "credential",
      "rm -rf", "format"};
};

struct GateVerdict {
  bool allowed = false;  ///< payload passed schema + policy
  /// "OK" | "PAYLOAD_SCHEMA_INVALID" | "TOOL_NOT_ALLOWED" |
  /// "DESTRUCTIVE_PROPOSE_ONLY" (allowed=true, propose-only per A.3)
  std::string code;
  std::string message;  ///< fail-loud: names the rule + recovery
};

/// Validate one leaf-proposed tool payload. Rules, in strict order:
///  (a) schema: payload is an object with "tool" (string) + "args"
///      (object); anything else => {false, PAYLOAD_SCHEMA_INVALID}.
///  (b) allowlist: tool not in policy.allowedTools =>
///      {false, TOOL_NOT_ALLOWED} — checked BEFORE the destructive scan
///      so an unknown destructive tool is REFUSED, not proposed (F21).
///  (c) destructive-verb scan over the tool name and every string inside
///      args (recursive): hit => {true, DESTRUCTIVE_PROPOSE_ONLY} —
///      propose-only, must not auto-execute (spec A.3).
///  (d) otherwise {true, OK}.
GateVerdict checkPayload(const nlohmann::json& payload, const PolicyConfig& policy);

/// True only for verdicts that may reach SwarmSpawner::spawn WITHOUT a
/// human approval step: allowed && code == "OK".
bool autoExecutable(const GateVerdict& v);

/// Host helper for the call-site contract: throws std::runtime_error on
/// !allowed (refuse + DENY ledger line is the caller's job); returns
/// autoExecutable(v) — false means propose-only (destructive), the host
/// MUST NOT spawn. Fail-loud message names the code and recovery.
bool enforceGateBeforeSpawn(const GateVerdict& v);

}  // namespace dshlite
