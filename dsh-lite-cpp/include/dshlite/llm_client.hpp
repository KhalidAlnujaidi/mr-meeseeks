#pragma once
// llm_client.hpp — Module 3a: Executive LLM client (SRS Milestone 3).
//
// Non-blocking HTTPS POST to OpenAI-compliant endpoints (OpenRouter
// default) via cpp-httplib + OpenSSL. Strict token tracking: every
// response's usage block accumulates into totalUsage()/requestCount().
// Network layer only — BrainLoop owns the Message state array.

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
  std::string endpoint = "https://openrouter.ai/api/v1/chat/completions";
  std::string apiKeyEnv = "OPENROUTER_API_KEY";
  /// "auto" (default) = shuffle-pick a verified free model per call.
  /// Explicit ids MUST be free (end in ":free" or in kFreeModelPool);
  /// a paid id on a real endpoint falls back to a random free model.
  /// Loopback endpoints (127.0.0.1/localhost, i.e. unit tests) bypass
  /// the free-only gate so stub servers can assert payload shape.
  std::string model = "auto";
  int maxTokens = 1024;  ///< reasoning free models need room (>= 400)
  std::chrono::milliseconds timeout{60000};
  std::string apiKey;  ///< explicit key (tests); env var wins when set
};

/// Verified free-only pool (provenance: scripts/or-swarm FREE_MODELS,
/// verified=True live 2026-09-17, cost 0). Never add a paid id here.
inline const std::vector<std::string> kFreeModelPool = {
    "nex-agi/nex-n2.5-pro:free",
    "nex-agi/nex-n2.5-mini:free",
    "cohere/north-mini-code:free",
    "dots-studio/dots-3-note-preview:free",
    "inclusionai/ling-3.0-flash-vl:free",
    "inclusionai/ling-3.0-flash-sante:free",
};

namespace detail {
// Free-only model resolution, exposed for unit tests.
// "auto"/"" => shuffle-pick from kFreeModelPool. Explicit free ids pass.
// Loopback hosts (unit-test stubs) bypass so tests assert payload shape.
// Paid ids on real hosts fall back to a random free model — never spend.
std::string resolveModelForHost(const LlmConfig& cfg, const std::string& host);
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

  /// Synchronous POST. Throws std::runtime_error on missing key,
  /// transport failure, non-200 status, or malformed JSON.
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
