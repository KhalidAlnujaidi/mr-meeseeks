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
};

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
