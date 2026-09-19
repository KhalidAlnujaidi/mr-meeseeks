#pragma once
// llm_client.hpp — Module 3a: Executive LLM client (SRS Milestone 3).
//
// Colibri-only. POSTs OpenAI-compliant chat completions to a local
// `coli serve` engine (default http://127.0.0.1:8000/v1/chat/completions)
// via cpp-httplib (+ OpenSSL when the endpoint is https). Strict token
// tracking: every response's usage block accumulates into
// totalUsage()/requestCount(). Network layer only — BrainLoop owns the
// Message state array.
//
// Model policy: the engine answers 404 unless body.model equals its
// --model-id, so the client sends cfg.model VERBATIM (default
// "glm-5.3-flash-colibri", the smallest colibri family default). No
// shuffle, no fallback, no remote providers — the harness holds its
// own engine, it never rents intelligence behind an API.

#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace dshlite {

struct Message {
  std::string role;  ///< "system" | "user" | "assistant"
  std::string content;
};

struct TokenUsage {
  long promptTokens = 0;
  long completionTokens = 0;
  long totalTokens = 0;
};

struct LlmConfig {
  std::string endpoint = "http://127.0.0.1:8000/v1/chat/completions";
  /// Bearer key for non-loopback binds. Loopback needs none (colibri
  /// serves 127.0.0.1 without auth unless --api-key is set). Env var
  /// wins when set; explicit apiKey is the fallback (tests).
  std::string apiKeyEnv = "COLI_API_KEY";
  /// Sent VERBATIM as body.model. Must equal the engine's --model-id
  /// (else HTTP 404 model_not_found). Default: smallest colibri family.
  std::string model = "glm-5.3-flash-colibri";
  int maxTokens = 1024;
  std::chrono::milliseconds timeout{60000};
  std::string apiKey;
  /// SSE streaming (roadmap G3.1/F11): enables time-to-first-token
  /// measurement. Non-streaming responses report ttftMs = -1 — ttft is
  /// never fabricated when the whole body arrives at once.
  bool stream = false;
  /// Streaming liveness stall interval (roadmap G2.1/F6): with
  /// stream=true, abort the request when ZERO bytes arrive on the wire
  /// for this long. This is a no-bytes interval, NOT a wall-clock cap —
  /// a slow-but-alive 0.05 tok/s engine that keeps dripping bytes is
  /// never stalled. Implemented as the httplib per-recv read timeout
  /// (each received byte restarts the interval). 0 disables: the read
  /// timeout falls back to `timeout` (pre-G2.1 behavior).
  std::chrono::milliseconds stallNoBytesMs{0};
};

/// Colibri family model ids (provenance: colibri c/family_registry.py
/// default_model_id per family). Any --model-id the engine serves is
/// accepted — this list is documentation, NOT a gate.
inline const std::vector<std::string> kColibriModelIds = {
    "glm-5.3-flash-colibri",  "glm-5.2-colibri",       "inkling-colibri",
    "kimi-k3-colibri",        "olmoe-colibri",         "qwen3.6-colibri",
    "qwen3.8-flash-next-colibri", "deepseek-v4-colibri",
    "deepseek-v4.1-flash-colibri",
};

namespace detail {
// Loopback check, exposed for unit tests: loopback engines need no key.
bool isLoopbackHost(const std::string& host);
}  // namespace detail

struct LlmResponse {
  std::string content;
  TokenUsage usage;
  long latencyMs = 0;
  /// Time to first streamed content token, ms. -1 when not measured
  /// (non-streaming call, or a stream that produced no content).
  long ttftMs = -1;
  /// Streaming liveness (G2.1/F6): ms from request start to the LAST
  /// byte received on the wire. -1 when not measured (non-streaming, or
  /// no bytes ever arrived). Lets the router/host separate slow-but-
  /// alive from stalled WITHOUT relying on wall-clock alone.
  long lastByteMs = -1;
  /// Largest observed inter-byte silence gap on the wire, ms. -1 when
  /// not measured / fewer than one byte received (streaming only).
  long maxIdleMs = -1;
};

/// Stall abort (G2.1/F6): thrown by post() when a streaming request saw
/// ZERO bytes on the wire for cfg.stallNoBytesMs. Distinct type so the
/// router classifies the attempt as "stall-no-bytes" and falls through
/// to the next WORKER family — never the brain pool (F2). what() always
/// contains the literal marker "stall-no-bytes" plus the cap, the
/// measured silence, and the recovery (fail-loud house rule).
struct LlmStallError : std::runtime_error {
  LlmStallError(const std::string& msg, long silentMsIn, long lastByteMsIn,
                long bytesSeenIn)
      : std::runtime_error(msg),
        silentMs(silentMsIn),
        lastByteMs(lastByteMsIn),
        bytesSeen(bytesSeenIn) {}
  long silentMs;    ///< measured no-bytes interval that tripped the cap
  long lastByteMs;  ///< last wire byte, ms into stream (-1 = none ever)
  long bytesSeen;   ///< total wire chunks received before the stall
};

/// Decode velocity (roadmap G3.1/F13): completion tokens over DECODE
/// wall time (latency - ttft when streaming, full latency otherwise —
/// prefill/TTFT is disk-bound warmup, not decode). Returns 0.0 when
/// completionTokens is 0 (an empty completion is not a velocity signal,
/// F12) or the decode window is non-positive.
double decodeTokPerSec(const LlmResponse& r);

/// Transport seam: BrainLoop depends on this, tests inject fakes.
class ILlmPoster {
 public:
  virtual ~ILlmPoster() = default;
  virtual LlmResponse post(const std::vector<Message>& messages) = 0;
};

class LlmClient : public ILlmPoster {
 public:
  explicit LlmClient(LlmConfig cfg);

  /// Synchronous POST. No key is sent on loopback (local `coli serve`
  /// needs none unless --api-key is set). Throws std::runtime_error on
  /// transport failure, non-200 status (incl. 404 model_not_found when
  /// cfg.model != engine --model-id), or malformed JSON.
  LlmResponse post(const std::vector<Message>& messages) override;

  /// Non-blocking POST; exceptions surface on future::get().
  std::future<LlmResponse> postAsync(const std::vector<Message>& messages);

  TokenUsage totalUsage() const {
    std::lock_guard<std::mutex> l(mu_);
    return total_;
  }
  long requestCount() const {
    std::lock_guard<std::mutex> l(mu_);
    return requests_;
  }

 private:
  LlmConfig cfg_;
  mutable std::mutex mu_;  // guards total_/requests_ across postAsync threads
  TokenUsage total_{};
  long requests_ = 0;
};

}  // namespace dshlite
