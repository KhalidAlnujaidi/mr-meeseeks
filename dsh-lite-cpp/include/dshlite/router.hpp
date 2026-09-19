#pragma once
// router.hpp — Module 3c: Multi-Engine Local Router (roadmap Gap 1,
// docs/colibri-roadmap.md G1.1–G1.5).
//
// Routes each role (brain | worker | verifier) to an ordered pool of
// local `coli serve` engines, keyed by (endpoint, model-id) PAIRS —
// the engine 404s anything but its verbatim --model-id (F3), so a port
// alone is never a valid route.
//
// Fallback law (F2): a stalled/failing worker re-routes to the NEXT
// WORKER-FAMILY entry. The brain endpoint is never a fallback target
// for any other role; an exhausted pool escalates (throws) instead.
//
// Heterogeneity (G1.2, VISION.md constraint 2): worker and verifier
// pools with < 2 distinct model families produce a config warning —
// correlated blindness is a named risk, not a silent default.
//
// Thread safety (G1.4): one LlmClient per entry, created once; each
// LlmClient is internally mutex-guarded and builds a fresh httplib
// connection per call, so concurrent post()/postAsync() across roles
// and entries is race-free.

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "dshlite/llm_client.hpp"

namespace dshlite {

enum class Role { Brain, Worker, Verifier };

const char* roleName(Role r);
/// Throws std::invalid_argument on unknown names.
Role roleFromName(const std::string& name);

/// Family derivation (F3/G1.2): strip a trailing "-colibri", then take
/// the prefix up to the first digit/dash/dot. "glm-5.3-flash-colibri"
/// -> "glm"; "qwen3.8-flash-next-colibri" and "qwen3.6-colibri" both
/// -> "qwen" (same family — naive dash-split would wrongly count two).
std::string deriveFamily(const std::string& modelId);

struct EngineEntry {
  std::string endpoint;  ///< full URL, e.g. http://127.0.0.1:8081/v1/chat/completions
  std::string modelId;   ///< VERBATIM engine --model-id (404 otherwise)
  /// Optional; empty => deriveFamily(modelId). Set explicitly when a
  /// custom --model-id hides the family.
  std::string family;
  int maxTokens = 1024;
  std::chrono::milliseconds timeout{60000};
  /// Key material for non-loopback binds only; loopback needs none.
  std::string apiKey;
  std::string apiKeyEnv = "COLI_API_KEY";
  /// SSE streaming for ttft measurement (G3.1/F11). Off by default:
  /// engines that ignore `stream:true` still answer non-chunked.
  bool stream = false;
  /// Velocity floor (G3.2/F4): abort a response slower than this
  /// (decode tok/s, F13 window). 0.0 disables the check for the entry.
  /// Per-engine-profile by design — a cold 25 GB box and a warm
  /// 6x5090 host do not share a floor.
  double minTokPerSec = 0.0;
  /// Warmup exemption (G3.2/F4): the first N successful+failed turns
  /// on this entry are NEVER velocity-aborted; cold NVMe prefill and
  /// .coli_usage pinning ramp is expected physics, not a fault.
  int warmupTurns = 5;
  /// Streaming liveness stall interval (G2.1/F6): with stream=true, zero
  /// bytes on the wire for this long = STALL ("stall-no-bytes" attempt
  /// outcome, worker->worker fallthrough per F2). Default 120 s per the
  /// roadmap; 0 disables (read timeout falls back to `timeout`). NEVER
  /// a wall-clock cap: a 0.05 tok/s cold engine that keeps dripping
  /// bytes is slow-but-alive, not stalled. LAST FIELD: tests brace-init
  /// EngineEntry positionally (F18) — appending keeps them valid.
  std::chrono::milliseconds stallNoBytesMs{120000};
};

struct RouterConfig {
  std::vector<EngineEntry> brain;     ///< exactly the frontier engine(s)
  std::vector<EngineEntry> worker;    ///< heterogeneous leaf pool
  std::vector<EngineEntry> verifier;  ///< heterogeneous check pool
};

/// One routing attempt, for the ledger v2 `route` lines (Gap 3/4).
struct RouteAttempt {
  std::string endpoint;
  std::string modelId;
  /// "ok" | "transport" (incl. timeout) | "stall-no-bytes" (G2.1/F6:
  /// streaming liveness stall — zero wire bytes for stallNoBytesMs) |
  /// "http-404" | "http-4xx" | "http-5xx" | "malformed" |
  /// "velocity-floor" | "error"
  std::string outcome;
  std::string detail;  ///< exception text, caller sanitizes before ledger
};

struct RoutedResponse {
  LlmResponse response;
  std::string servedBy;  ///< endpoint that answered
  std::string modelId;   ///< model-id that answered
  std::vector<RouteAttempt> attempts;  ///< >= 1; last is "ok"
};

class ModelRouter {
 public:
  /// Throws std::invalid_argument on hard config errors (G1.1/G1.3):
  /// empty pool, empty modelId, bad endpoint scheme, duplicate endpoint
  /// within a pool, or a brain endpoint appearing in worker/verifier
  /// pools (brain is never reachable by another role).
  explicit ModelRouter(RouterConfig cfg);

  /// G1.2 warnings, e.g. single-family worker pool. Surfaced by callers
  /// on every report per the roadmap.
  const std::vector<std::string>& warnings() const { return warnings_; }

  /// Ordered-pool dispatch with worker->worker fallback (G1.3).
  /// Throws std::runtime_error("router: <role> pool exhausted ...")
  /// when every entry failed — escalate, never touch the brain pool.
  RoutedResponse post(Role role, const std::vector<Message>& messages);

  /// G2.4: same dispatch under a response_format (grammar-forced
  /// DRAFTS — F45: an accelerator, never an output guarantee; the host
  /// gate stays the enforcement). F46: an engine whose family refuses
  /// the grammar (HTTP 400 unsupported_parameter) records attempt
  /// outcome "grammar-unsupported" and the pool falls through to the
  /// next family; if EVERY entry refused on grammar grounds the throw
  /// cites the grammar rejection (fail-loud — misconfiguration must
  /// not hide behind a generic transport error).
  RoutedResponse postConstrained(Role role,
                                 const std::vector<Message>& messages,
                                 const ResponseFormat& rf);

  /// Non-blocking variant; exceptions surface on future::get().
  std::future<RoutedResponse> postAsync(Role role,
                                        std::vector<Message> messages);

  /// Live probe (G1.1): one throwaway request per entry verifying the
  /// endpoint answers its own model-id. Uses isolated clients, so probe
  /// traffic never pollutes totalUsage()/requestCount().
  struct ProbeResult {
    std::string endpoint;
    std::string modelId;
    bool ok = false;
    std::string detail;
  };
  std::vector<ProbeResult> probe();

  /// Aggregates across every pooled client (mutex-guarded per client).
  TokenUsage totalUsage() const;
  long requestCount() const;

 private:
  const std::vector<EngineEntry>& poolFor(Role role) const;
  /// Shared dispatch for post()/postConstrained(): rf == nullptr sends
  /// the plain wire; non-null passes the response_format to each
  /// pooled client (G2.4).
  RoutedResponse postImpl_(Role role, const std::vector<Message>& messages,
                           const ResponseFormat* rf);
  /// Offset of the role's pool inside clients_/turnCounters_.
  size_t poolOffset_(Role role) const {
    switch (role) {
      case Role::Brain: return 0;
      case Role::Worker: return brainN_;
      case Role::Verifier: return brainN_ + workerN_;
    }
    return 0;
  }
  LlmClient& clientFor(Role role, size_t idx);

  RouterConfig cfg_;
  /// Clients laid out [brain..., worker..., verifier...]; built once.
  std::vector<std::unique_ptr<LlmClient>> clients_;
  size_t brainN_ = 0, workerN_ = 0;  ///< pool offsets
  std::vector<std::string> warnings_;
  /// Per-entry completed-turn counters (F14): warmup exemption state.
  /// Same [brain..., worker..., verifier...] layout as clients_.
  /// Atomic: concurrent dispatch (G1.4) must not race the exemption.
  std::vector<std::unique_ptr<std::atomic<long>>> turnCounters_;

  /// G3.2 velocity floor: returns true when `resp` from entry `e`
  /// (turns already completed: `turnsSeen`) must be ABORTED. Exempt
  /// while turnsSeen < e.warmupTurns (F4) or when the floor is 0.0
  /// (disabled) or completionTokens == 0 (no velocity signal, F12).
  static bool velocityAborts(const EngineEntry& e, const LlmResponse& resp,
                             long turnsSeen);
};

}  // namespace dshlite
