#pragma once
// compaction.hpp — Gap 2 (G2.5/F1): context compaction + E.2.3 result
// vectors for BrainLoop::history_ (a std::vector<Message> — NO deque
// exists anywhere in this codebase and none is introduced, F1).
//
// Two jobs:
//  1. compactHistory(): when serialized history exceeds thresholdChars
//     (default 8192, mirroring the preset/budget-agi tool-result-pruner
//     budget), prune MIDDLE turns — never the system message (index 0),
//     never the final message, never the last user message. Per-message
//     head/tail truncation (headChars 4096 + tailChars 1024) runs first
//     as a bounded second stage; whole-turn pruning is the load-bearing
//     mechanism (F23: the Module 1 sanitizer caps any single message at
//     ~4096 chars BEFORE compaction truncation could ever bite at its
//     default sizes — documented, not silently relied on).
//  2. toResultVector(): normalize + validate a leaf result vector to the
//     spec E.2.3 schema (docs/budget-agi-host-middleware-spec.md):
//     uppercase canonical status, 1-5 line summary, repo-relative
//     changedFiles, conditional gitDiffHash, scalar-only exportedState
//     <= 4 KiB — oversize/non-scalar => BUS_VALUE_TOO_LARGE refuse,
//     NEVER truncate (spec: truncate-with-flag was considered and
//     REJECTED).
//
// Every string this module re-injects into history or emits in a result
// vector passes through the Module 1 sanitizer (dshlite/sanitize).

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/llm_client.hpp"  // Message

namespace dshlite {

struct CompactionConfig {
  long thresholdChars = 8192;  ///< serialized history budget
  long headChars = 4096;       ///< per-message head kept when truncating
  long tailChars = 1024;       ///< per-message tail kept when truncating
};

/// Serialized size of a history: sum(role + content + 4 framing chars).
long serializedChars(const std::vector<Message>& h);

/// Prune `h` in place until serializedChars(h) <= cfg.thresholdChars or
/// nothing prunable remains. Returns true when anything changed.
/// Laws: index 0 (system), the final message, and the LAST user message
/// are NEVER touched (G2.5). Pruned middle turns collapse into one
/// host-attributed "[HOST COMPACTION]" marker (role "user", content
/// sanitized) recording the count — provenance is explicit, never
/// disguised as model output. When even the protected messages exceed
/// the threshold, compaction stops honestly: the return value says
/// "changed", serializedChars() says whether the budget was met — this
/// function never lies about reaching the threshold.
bool compactHistory(std::vector<Message>& h, const CompactionConfig& cfg);

/// Marker text prefix for compacted regions (provenance, G2.5).
inline constexpr const char* kCompactionMarker = "[HOST COMPACTION]";

// ── E.2.3 result vector ────────────────────────────────────────────────

/// Canonical result-vector statuses (uppercase by convention; lowercase
/// input is normalized, anything else rejected — spec E.2.3).
inline constexpr const char* kStatusSuccess = "SUCCESS";
inline constexpr const char* kStatusFailed = "FAILED";
inline constexpr const char* kStatusDiscarded = "DISCARDED";

struct ResultVector {
  std::string taskId;
  std::string status;  ///< SUCCESS | FAILED | DISCARDED (canonical)
  std::string summary;  ///< 1-5 lines, sanitized
  std::vector<std::string> changedFiles;  ///< repo-relative, may be empty
  std::string gitDiffHash;  ///< "" iff no patch exists (E.2.3 conditional)
  nlohmann::json exportedState = nlohmann::json::object();  ///< scalars only
};

struct ResultVectorVerdict {
  bool ok = false;
  /// "" on success; else "BUS_VALUE_TOO_LARGE" (exportedState oversize or
  /// non-scalar — refuse, never truncate) or "VECTOR_FIELD_INVALID"
  /// (bad/missing taskId, status, summary length, changedFiles, or
  /// gitDiffHash↔patch binding).
  std::string code;
  std::string message;  ///< fail-loud: names field + rule + recovery
  ResultVector vector;  ///< populated only when ok
};

/// Validate + normalize a leaf-submitted result vector (JSON in the
/// E.2.3 shape). `hasPatchFile` = patches/<task>.patch exists on disk:
/// gitDiffHash is REQUIRED non-empty iff true, and "" is the canonical
/// value when false (B.5/D.4 withhold cases carry the reason in
/// summary). exportedState: object of string/number/bool only, <= 4 KiB
/// serialized; violations => BUS_VALUE_TOO_LARGE, vector unchanged.
ResultVectorVerdict toResultVector(const nlohmann::json& in, bool hasPatchFile);

/// Serialize a validated vector to the E.2.3 wire JSON.
nlohmann::json resultVectorJson(const ResultVector& v);

/// 4 KiB exportedState cap (spec: resultVector.exportedStateMaxBytes).
inline constexpr long kExportedStateMaxBytes = 4096;

}  // namespace dshlite
