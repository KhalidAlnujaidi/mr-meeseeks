#pragma once
// ledger.hpp — Module 4: ledger.jsonl v2 emitter (roadmap G3.1/G4.1,
// schema in docs/colibri-roadmap.md). Native C++ emission: wall-clock
// metrics captured at the socket boundary, zero TS runtime dependency
// (Ownership decision, F9/F10).
//
// Thread safety (F15): one mutex guards every append; each line is
// serialized fully in memory, then written with a single write(2) to an
// O_APPEND fd — kernel-atomic for lines under PIPE_BUF on local
// filesystems, so concurrent threads can never interleave partial JSON.
// A failed write throws std::runtime_error (fail loud, never silent).

#include <mutex>
#include <string>

#include "dshlite/llm_client.hpp"
#include "dshlite/router.hpp"
#include "dshlite/usage_probe.hpp"

namespace dshlite {

/// Schema version stamped on every line ("schema": 2).
inline constexpr int kLedgerSchema = 2;

/// One ledger event. Fields left at defaults are omitted from the JSON
/// (null vs absent: `ttftMs < 0` emits "ttft_ms": null per the v2
/// schema; empty optional strings are omitted entirely).
struct LedgerEvent {
  std::string type;       // split|report|verify|DENY|nudge|route
  std::string taskId;
  std::string parentId;   // omitted when empty
  int depth = 0;
  std::string role;       // brain|worker|verifier|captain|reviewer
  std::string model;      // model-id that served (or was attempted)
  std::string endpoint;   // loopback endpoint, host:port form preferred
  // cost block (from LlmResponse; zeros are emitted as 0, not omitted)
  long promptTokens = 0;
  long completionTokens = 0;
  long latencyMs = -1;    // < 0 => omitted (no socket measurement)
  long ttftMs = -1;       // < 0 => "ttft_ms": null (non-streaming, F11)
  double tokPerSec = 0.0; // 0.0 => omitted (no velocity signal, F12)
  /// F35 provenance: true => token counts are client-side delta-count
  /// estimates (engine omitted the usage chunk). Emitted as
  /// cost.tokens_estimated ONLY when true — engine-authoritative usage
  /// needs no flag.
  bool tokensEstimated = false;
  // route/DENY/nudge specifics
  std::string verdict;    // verify lines: pass|fail
  std::string gate;       // DENY lines: SPAWN|SPLIT|TURN|VELOCITY
  std::string code;       // DENY lines: VELOCITY_FLOOR | BUDGET_EXCEEDED | ...
  int nudgeDepth = -1;    // nudge lines; < 0 => omitted
  int resplitCount = -1;  // < 0 => omitted
  std::string detail;     // SANITIZED by the writer (Module 1), never raw
  bool cacheWarm = false; // best-effort .coli_usage heat (F5); see hasCache
  bool hasCache = false;  // false => "cache" block omitted entirely
};

/// Append-only JSONL writer. Copy/move disabled (owns the fd).
class LedgerWriter {
 public:
  /// Opens `path` with O_APPEND|O_CREAT|O_WRONLY (0644). Throws
  /// std::runtime_error when the file cannot be opened.
  explicit LedgerWriter(const std::string& path);
  ~LedgerWriter();

  LedgerWriter(const LedgerWriter&) = delete;
  LedgerWriter& operator=(const LedgerWriter&) = delete;

  /// Serialize + append one event as a single write(2). `ts` is
  /// host-assigned UTC ISO-8601 (workers never supply timestamps).
  void append(const LedgerEvent& ev);

  /// Convenience: build a `report`-shaped event from a routed response
  /// (fills cost block + tok/s from decodeTokPerSec, F13).
  static LedgerEvent fromRouted(const std::string& type, const std::string& role,
                                const std::string& taskId, int depth,
                                const RoutedResponse& r);

  /// Pure serializer, exposed for tests: returns the exact JSON line
  /// (no trailing newline) that append() would write.
  static std::string serialize(const LedgerEvent& ev, const std::string& tsIso);

  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
  std::mutex mu_;  // F15: guards serialize+write; one write(2) per line
};

/// UTC ISO-8601 with millisecond precision: 2026-09-19T12:00:00.000Z
std::string utcNowIso();

/// Heat -> cache.warm wiring (roadmap G3.3/F5/F39): attach a P1
/// UsageProbe snapshot to an event. probe.ok => cache block emitted
/// with warm=probe.warm; !ok (missing/unparsable file) => the block is
/// OMITTED entirely, never warm:false — absence means "no heat data",
/// not "cold". Best-effort telemetry ONLY: nothing may gate on this
/// until the roadmap A/B says so (F5 law).
void attachCacheHeat(LedgerEvent& ev, const UsageProbe& probe);

}  // namespace dshlite
