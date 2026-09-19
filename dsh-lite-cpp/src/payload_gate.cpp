// payload_gate.cpp — Gap 2 (G2.4/F7): pre-execution payload gate.
// See include/dshlite/payload_gate.hpp for the contract + call-site law.

#include "dshlite/payload_gate.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace dshlite {
namespace {

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// Word-boundary containment (F21): `verb` must appear in `hay` bounded
// by non-alphanumerics (or string ends). '_' and '-' and ' ' all count
// as boundaries, so "delete_branch" and "git push" match "delete"/"push"
// while "information" never matches "format". Naive substring scans
// demote benign payloads; this keeps the scan honest AND conservative.
bool containsVerb(const std::string& hayLower, const std::string& verbLower) {
  if (verbLower.empty()) return false;
  size_t pos = 0;
  while ((pos = hayLower.find(verbLower, pos)) != std::string::npos) {
    const bool leftOk =
        pos == 0 || !std::isalnum(static_cast<unsigned char>(hayLower[pos - 1]));
    const size_t end = pos + verbLower.size();
    const bool rightOk =
        end >= hayLower.size() ||
        !std::isalnum(static_cast<unsigned char>(hayLower[end]));
    if (leftOk && rightOk) return true;
    pos = end;
  }
  return false;
}

bool isDestructiveText(const std::string& text,
                       const std::vector<std::string>& verbs) {
  const std::string lower = toLower(text);
  for (const auto& v : verbs) {
    if (containsVerb(lower, toLower(v))) return true;
  }
  return false;
}

// Recursive scan of every string KEY and string VALUE inside args.
bool anyDestructiveIn(const nlohmann::json& node,
                      const std::vector<std::string>& verbs) {
  if (node.is_string()) return isDestructiveText(node.get<std::string>(), verbs);
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (isDestructiveText(it.key(), verbs)) return true;
      if (anyDestructiveIn(it.value(), verbs)) return true;
    }
  }
  if (node.is_array()) {
    for (const auto& el : node)
      if (anyDestructiveIn(el, verbs)) return true;
  }
  return false;
}

}  // namespace

GateVerdict checkPayload(const nlohmann::json& payload, const PolicyConfig& policy) {
  GateVerdict v;
  // (a) Schema: object with "tool" (string) + "args" (object). F7 made
  // structured JSON payloads a PREREQUISITE — a payload that is not one
  // never reaches a subprocess.
  if (!payload.is_object()) {
    v.allowed = false;
    v.code = "PAYLOAD_SCHEMA_INVALID";
    v.message =
        "payload gate: leaf payload must be a JSON object with \"tool\" + "
        "\"args\" (F7/G2.4) — got " + std::string(payload.type_name()) +
        "; recovery: re-prompt the leaf for a grammar-forced JSON payload";
    return v;
  }
  if (!payload.contains("tool") || !payload["tool"].is_string()) {
    v.allowed = false;
    v.code = "PAYLOAD_SCHEMA_INVALID";
    v.message =
        "payload gate: required field \"tool\" (string) missing — recovery: "
        "re-prompt the leaf (retry within I6 budget, then escalate)";
    return v;
  }
  if (!payload.contains("args") || !payload["args"].is_object()) {
    v.allowed = false;
    v.code = "PAYLOAD_SCHEMA_INVALID";
    v.message =
        "payload gate: required field \"args\" (object) missing for tool \"" +
        payload["tool"].get<std::string>() +
        "\" — recovery: re-prompt the leaf (retry within I6 budget, then escalate)";
    return v;
  }
  const std::string tool = payload["tool"].get<std::string>();
  // (b) Allowlist BEFORE the destructive scan (F21 ordering): an unknown
  // tool is REFUSED even when its name screams "delete" — a destructive
  // verb never launders an unlisted tool into propose-only.
  if (std::find(policy.allowedTools.begin(), policy.allowedTools.end(), tool) ==
      policy.allowedTools.end()) {
    v.allowed = false;
    v.code = "TOOL_NOT_ALLOWED";
    v.message = "payload gate: tool \"" + tool +
                "\" is not in the host allowlist (" +
                std::to_string(policy.allowedTools.size()) +
                " allowed) — refused; recovery: propose an allowlisted tool "
                "or ask the host to extend PolicyConfig.allowedTools";
    return v;
  }
  // (c) Destructive-verb scan: tool name + every string in args. Hit =>
  // propose-only (spec A.3 DESTRUCTIVE_PROPOSE_ONLY) — allowed to be
  // PROPOSED, never auto-executed without explicit user approval.
  if (isDestructiveText(tool, policy.destructiveVerbs) ||
      anyDestructiveIn(payload["args"], policy.destructiveVerbs)) {
    v.allowed = true;
    v.code = "DESTRUCTIVE_PROPOSE_ONLY";
    v.message = "proposed only, not enqueued (destructive): tool \"" + tool +
                "\" — needs explicit user approval (spec A.3); the host MUST "
                "NOT pass this to SwarmSpawner::spawn automatically";
    return v;
  }
  // (d) Clean.
  v.allowed = true;
  v.code = "OK";
  v.message = "";
  return v;
}

bool autoExecutable(const GateVerdict& v) {
  return v.allowed && v.code == "OK";
}

bool enforceGateBeforeSpawn(const GateVerdict& v) {
  if (!v.allowed)
    throw std::runtime_error(
        "payload gate: REFUSED (" + v.code + ") — spawn forbidden; write a "
        "DENY ledger line {gate:\"SPAWN\", code:\"" + v.code +
        "\"} and retry within the I6 budget, then escalate. " + v.message);
  return autoExecutable(v);  // false => propose-only, do NOT spawn
}

}  // namespace dshlite
