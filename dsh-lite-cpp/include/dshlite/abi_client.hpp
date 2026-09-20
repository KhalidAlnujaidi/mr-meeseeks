#pragma once
// abi_client.hpp — native Colibri C ABI engine backend (direct in-process
// decode through libcolibri_segment_edge.a; NO coli serve, NO Python, NO
// HTTP, zero IPC).
//
// Implements ILlmPoster so BrainLoop/the router can drive it exactly like
// LlmClient. Pipeline per post(): flatten messages -> tokenize (edge) ->
// embed (edge) -> segment_run over the full layer range -> greedy select
// (edge) -> detokenize (edge).
//
// Native-lane contracts (honest deltas vs the HTTP lane):
//  - RAM boundary: AbiConfig.memoryLimitBytes is wired DIRECTLY into
//    ColiEdgeEngineOptions/ColiSegmentEngineOptions memory_limit_bytes —
//    enforced natively at engine open, not by an external supervisor.
//  - Wall-clock firewall: cfg.timeout becomes a steady_clock deadline;
//    the ABI should_cancel callback (wired into embed/run/select) polls
//    it in-process, so an over-budget decode aborts deterministically
//    with AbiCancelledError — zero IPC, no signals, no kill -9.
//  - F80 flattening: no chat template on this lane. Messages are
//    flattened as "role: content" lines. Serve mode applies the model's
//    chat template; the native lane does not. Contract delta, documented.
//  - F76 EOS: stop-on-EOS only when the adapter advertises
//    eos_token_id >= 0 (OLMoE edge caps report -1); otherwise decode
//    runs to maxTokens. Never a hardcoded stop id.
//  - Usage: promptTokens/completionTokens are REAL in-process counts of
//    our own tokenize/decode steps — usageEstimated=false is honest
//    provenance, not engine telemetry. ttftMs = time to first generated
//    token (measured in-process). lastByteMs/maxIdleMs stay -1 (no wire).
//  - Grammar: the C ABI exposes no response_format path, so
//    postConstrained throws GrammarUnsupportedError — the same typed
//    refusal the router already classifies (F46 taxonomy).
//
// Flaw audit F74-F80: see bench/h2h/README.md discipline; mitigations:
//  F74 idempotent registration, F75 AbiContextOverflowError preflight,
//  F76 caps-driven EOS, F77 session-per-call (cancel-safe), F78 empty
//  detokenize tolerated, F79 activation-size overflow checked.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "dshlite/llm_client.hpp"

namespace dshlite {

struct AbiConfig {
  std::string modelDir;                 ///< model weights directory
  std::string engineId = "olmoe";       ///< adapter family id
  /// Wired directly into edge+segment opts.memory_limit_bytes (native
  /// RAM boundary). 0 = adapter automatic budget.
  std::uint64_t memoryLimitBytes = 8ull * 1024 * 1024 * 1024;
  int maxTokens = 256;                  ///< decode budget per post()
  /// Wall-clock firewall: the whole post() (prefill + decode) must fit
  /// inside this deadline; the should_cancel callback aborts it.
  std::chrono::milliseconds timeout{120000};
  bool stopOnEos = true;                ///< honored only when caps.eos >= 0 (F76)
};

/// Base type for every native-lane failure. what() carries the ABI
/// error buffer verbatim (fail-loud house rule).
struct AbiError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// Wall-clock firewall tripped (cfg.timeout). Distinct type so callers
/// can classify a budget abort separately from an engine failure.
struct AbiCancelledError : AbiError {
  using AbiError::AbiError;
};

/// F75: prompt + maxTokens exceeds the adapter's max_context_tokens.
/// Thrown BEFORE any session opens — never mid-decode.
struct AbiContextOverflowError : AbiError {
  AbiContextOverflowError(std::uint64_t neededIn, std::uint64_t capIn)
      : AbiError("context overflow: need " + std::to_string(neededIn) +
                 " tokens, adapter max " + std::to_string(capIn)),
        needed(neededIn), cap(capIn) {}
  std::uint64_t needed;
  std::uint64_t cap;
};

/// Wall-clock firewall state handed to the ABI should_cancel callback.
/// Public + trivial so tests can exercise the exact callback the engine
/// polls (no friend hacks, no mocks).
struct WallClockFirewall {
  std::chrono::steady_clock::time_point deadline;
  std::atomic<bool> tripped{false};
  /// ABI signature: int(void*). Returns 1 (abort) once past deadline.
  static int hit(void* userData);
};

class AbiClient : public ILlmPoster {
 public:
  /// Opens edge+segment engines against cfg.modelDir. Throws AbiError
  /// (with the engine's verbatim message) when registration or open
  /// fails. Engines live for the client's lifetime; sessions are
  /// per-call (F77 cancel safety).
  explicit AbiClient(AbiConfig cfg);
  ~AbiClient() override;
  AbiClient(const AbiClient&) = delete;
  AbiClient& operator=(const AbiClient&) = delete;

  /// Synchronous in-process decode. Throws AbiCancelledError (firewall),
  /// AbiContextOverflowError (F75 preflight), or AbiError (engine).
  LlmResponse post(const std::vector<Message>& messages) override;

  /// The native lane has no response_format path: typed refusal, same
  /// taxonomy as the HTTP lane's GrammarUnsupportedError (F46).
  LlmResponse postConstrained(const std::vector<Message>& messages,
                              const ResponseFormat& rf) override;

  /// F80 flattening, exposed for tests: "role: content" lines, in order.
  static std::string flattenMessages(const std::vector<Message>& messages);

  /// Per-call firewall budget is derived from cfg.timeout at each post();
  /// adjust it at runtime (e.g. F77 recovery tests). Thread-safety: same
  /// contract as post() itself — one caller at a time.
  void setTimeout(std::chrono::milliseconds t) { cfg_.timeout = t; }

  /// F74: idempotent adapter registration (mutex + set). Public for
  /// tests; safe to call repeatedly and from multiple threads.
  static void ensureAdapters(const std::string& engineId);

  // Telemetry (valid after construction).
  std::uint32_t vocabSize() const { return vocabSize_; }
  std::uint32_t numLayers() const { return numLayers_; }
  std::uint32_t stateWidth() const { return stateWidth_; }
  std::uint32_t maxContextTokens() const { return maxContextTokens_; }
  std::int32_t eosTokenId() const { return eosTokenId_; }
  std::uint64_t memoryLimitBytes() const { return cfg_.memoryLimitBytes; }

 private:
  struct Impl;  // owns the opaque C ABI engine handles
  AbiConfig cfg_;
  std::unique_ptr<Impl> impl_;
  std::uint32_t vocabSize_ = 0;
  std::uint32_t numLayers_ = 0;
  std::uint32_t stateWidth_ = 0;
  std::uint32_t maxContextTokens_ = 0;
  std::int32_t eosTokenId_ = -1;
};

}  // namespace dshlite
