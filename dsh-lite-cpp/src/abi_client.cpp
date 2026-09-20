// abi_client.cpp — native Colibri C ABI backend implementation.
// Decode loop lifted from the proven src/abi_probe.cpp spike; lifecycle
// reshaped for ILlmPoster use: engines live for the client, sessions are
// per-call (F77 — a firewall abort cannot corrupt long-lived state).

#include "dshlite/abi_client.hpp"

#include <cstring>

extern "C" {
#include "edge_adapters.h"
#include "edge_runtime.h"
#include "segment_adapters.h"
#include "segment_runtime.h"
}

namespace dshlite {
namespace {

std::mutex& registryMutex() {
  static std::mutex m;
  return m;
}
std::set<std::string>& registeredIds() {
  static std::set<std::string> ids;
  return ids;
}

int registerFamily(const std::string& engineId) {
  // Add families here as the native lane grows; each registration is
  // done exactly once per process (F74 idempotence handled by caller).
  if (engineId == "olmoe") {
    if (coli_olmoe_segment_adapter_register() != 0) return 1;
    if (coli_olmoe_edge_adapter_register() != 0) return 2;
    return 0;
  }
  return 3;  // unknown family
}

}  // namespace

int WallClockFirewall::hit(void* userData) {
  auto* self = static_cast<WallClockFirewall*>(userData);
  if (self == nullptr) return 0;
  if (std::chrono::steady_clock::now() >= self->deadline) {
    self->tripped.store(true, std::memory_order_relaxed);
    return 1;  // non-zero => abort the in-flight ABI call
  }
  return 0;
}

void AbiClient::ensureAdapters(const std::string& engineId) {
  std::lock_guard<std::mutex> lock(registryMutex());
  if (registeredIds().count(engineId) != 0) return;  // F74: idempotent
  const int rc = registerFamily(engineId);
  if (rc != 0) {
    throw AbiError("adapter registration failed for '" + engineId +
                   "' (rc=" + std::to_string(rc) +
                   (rc == 3 ? ", unknown family)" : ")"));
  }
  registeredIds().insert(engineId);
}

std::string AbiClient::flattenMessages(const std::vector<Message>& messages) {
  // F80: no chat template on the native lane. Documented flattening.
  std::string flat;
  for (const auto& m : messages) {
    if (!flat.empty()) flat += "\n";
    flat += m.role;
    flat += ": ";
    flat += m.content;
  }
  return flat;
}

struct AbiClient::Impl {
  ColiEdgeEngine* edge = nullptr;
  ColiSegmentEngine* segment = nullptr;
  ColiEdgeCapabilities edgeCap{};
};

AbiClient::AbiClient(AbiConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.modelDir.empty()) throw AbiError("modelDir is empty");
  if (cfg_.maxTokens < 1) throw AbiError("maxTokens must be >= 1");
  ensureAdapters(cfg_.engineId);

  impl_ = std::make_unique<Impl>();
  char error[512] = {0};

  // Edge engine — memory_limit_bytes wired straight from C++ config
  // (native RAM boundary, no external supervisor).
  ColiEdgeEngineOptions edgeOpts{};
  edgeOpts.struct_size = sizeof(edgeOpts);
  edgeOpts.model_dir = cfg_.modelDir.c_str();
  edgeOpts.memory_limit_bytes = cfg_.memoryLimitBytes;
  if (coli_edge_engine_open(cfg_.engineId.c_str(), &edgeOpts, &impl_->edge,
                            error, sizeof(error)) != 0) {
    throw AbiError(std::string("coli_edge_engine_open: ") + error);
  }
  impl_->edgeCap.struct_size = sizeof(ColiEdgeCapabilities);
  if (coli_edge_engine_capabilities(impl_->edge, &impl_->edgeCap, error,
                                    sizeof(error)) != 0) {
    const std::string msg = error;
    coli_edge_engine_close(impl_->edge);
    impl_->edge = nullptr;
    throw AbiError("coli_edge_engine_capabilities: " + msg);
  }
  vocabSize_ = impl_->edgeCap.vocab_size;
  numLayers_ = impl_->edgeCap.num_layers;
  stateWidth_ = impl_->edgeCap.state_width;
  maxContextTokens_ = impl_->edgeCap.max_context_tokens;
  eosTokenId_ = impl_->edgeCap.eos_token_id;
  if (!(impl_->edgeCap.flags & COLI_EDGE_CAP_TOKENIZE) ||
      !(impl_->edgeCap.flags & COLI_EDGE_CAP_GREEDY)) {
    coli_edge_engine_close(impl_->edge);
    impl_->edge = nullptr;
    throw AbiError("adapter lacks tokenize/greedy capabilities");
  }

  // Segment engine — all layers, same explicit memory budget. Opened
  // once; sessions are created per call (F77).
  ColiSegmentEngineOptions segOpts{};
  segOpts.struct_size = sizeof(segOpts);
  segOpts.model_dir = cfg_.modelDir.c_str();
  segOpts.layer_begin = 0;
  segOpts.layer_end = numLayers_;
  segOpts.context_tokens = maxContextTokens_;
  segOpts.memory_limit_bytes = cfg_.memoryLimitBytes;
  if (coli_segment_engine_open(cfg_.engineId.c_str(), &segOpts,
                               &impl_->segment, error, sizeof(error)) != 0) {
    const std::string msg = error;
    coli_edge_engine_close(impl_->edge);
    impl_->edge = nullptr;
    throw AbiError("coli_segment_engine_open: " + msg);
  }
}

AbiClient::~AbiClient() {
  if (!impl_) return;
  if (impl_->segment != nullptr) {
    char error[512] = {0};
    (void)coli_segment_engine_close(impl_->segment, error, sizeof(error));
  }
  if (impl_->edge != nullptr) coli_edge_engine_close(impl_->edge);
}

LlmResponse AbiClient::postConstrained(const std::vector<Message>&,
                                       const ResponseFormat&) {
  // Native lane has no response_format path — typed refusal, F46
  // taxonomy so the router classifies it exactly like the HTTP 400.
  throw GrammarUnsupportedError(
      "abi_client: response_format grammars are not supported by the "
      "native C ABI lane (no wire format)");
}

LlmResponse AbiClient::post(const std::vector<Message>& messages) {
  const auto start = std::chrono::steady_clock::now();
  WallClockFirewall fw;
  fw.deadline = start + cfg_.timeout;
  char error[512] = {0};
  LlmResponse resp;

  const std::string flat = flattenMessages(messages);
  if (flat.empty()) throw AbiError("post: empty message set");

  // Tokenize (sizing pass then fill — ABI contract).
  size_t promptCount = 0;
  if (coli_edge_tokenize(impl_->edge, flat.c_str(), flat.size(), nullptr, 0,
                         &promptCount, error, sizeof(error)) != 0) {
    throw AbiError(std::string("tokenize (sizing): ") + error);
  }
  if (promptCount == 0) throw AbiError("tokenize: empty prompt");

  // F75 preflight: refuse BEFORE opening a session, never mid-decode.
  const std::uint64_t needed = static_cast<std::uint64_t>(promptCount) +
                               static_cast<std::uint64_t>(cfg_.maxTokens) + 2;
  if (needed > maxContextTokens_) throw AbiContextOverflowError(needed, maxContextTokens_);

  std::vector<std::int32_t> promptIds(promptCount);
  if (coli_edge_tokenize(impl_->edge, flat.c_str(), flat.size(),
                         promptIds.data(), promptCount, &promptCount, error,
                         sizeof(error)) != 0) {
    throw AbiError(std::string("tokenize (fill): ") + error);
  }

  // Per-call session (F77): a firewall abort destroys ONLY this session.
  ColiSegmentSessionOptions sessOpts{};
  sessOpts.struct_size = sizeof(sessOpts);
  sessOpts.context_tokens = static_cast<std::uint32_t>(needed);
  ColiSegmentSession* session = nullptr;
  if (coli_segment_session_create(impl_->segment, &sessOpts, &session, error,
                                  sizeof(error)) != 0) {
    throw AbiError(std::string("session_create: ") + error);
  }
  // Session guard: destroyed on EVERY exit path, including throws.
  struct SessionGuard {
    ColiSegmentSession* s;
    ~SessionGuard() { coli_segment_session_destroy(s); }
  } guard{session};

  // F79: activation buffer overflow check before allocation.
  const size_t rowBytes = static_cast<size_t>(stateWidth_) * sizeof(float);
  if (promptCount > SIZE_MAX / rowBytes) {
    throw AbiError("prompt activation size overflows");
  }
  std::vector<float> in(promptCount * rowBytes / sizeof(float));
  std::vector<float> out(promptCount * rowBytes / sizeof(float));

  // Prefill: embed all prompt rows, run the full layer stack.
  ColiEdgeEmbedRequest embed{};
  embed.struct_size = sizeof(embed);
  embed.rows = static_cast<std::uint32_t>(promptCount);
  embed.token_ids = promptIds.data();
  embed.token_count = promptCount;
  embed.output = in.data();
  embed.output_bytes = in.size() * sizeof(float);
  embed.should_cancel = &WallClockFirewall::hit;
  embed.cancel_user_data = &fw;
  if (coli_edge_embed(impl_->edge, &embed, error, sizeof(error)) != 0) {
    if (fw.tripped.load()) throw AbiCancelledError("wall-clock firewall tripped during embed (prefill)");
    throw AbiError(std::string("embed (prefill): ") + error);
  }

  ColiSegmentRunRequest run{};
  run.struct_size = sizeof(run);
  run.rows = static_cast<std::uint32_t>(promptCount);
  run.position = 0;
  run.token_ids = promptIds.data();
  run.token_count = promptCount;
  run.input = in.data();
  run.input_bytes = in.size() * sizeof(float);
  run.output = out.data();
  run.output_bytes = out.size() * sizeof(float);
  run.should_cancel = &WallClockFirewall::hit;
  run.cancel_user_data = &fw;
  if (coli_segment_run(session, &run, error, sizeof(error)) != 0) {
    if (fw.tripped.load()) throw AbiCancelledError("wall-clock firewall tripped during segment_run (prefill)");
    throw AbiError(std::string("segment_run (prefill): ") + error);
  }

  // Select next token from the LAST prompt row (greedy, engine head).
  std::int32_t predicted = -1;
  float score = 0.0f;
  ColiEdgeSelectRequest select{};
  select.struct_size = sizeof(select);
  select.rows = 1;
  select.input = out.data() + (promptCount - 1) * stateWidth_;
  select.input_bytes = rowBytes;
  select.token_ids = &predicted;
  select.token_capacity = 1;
  select.scores = &score;
  select.score_capacity = 1;
  select.should_cancel = &WallClockFirewall::hit;
  select.cancel_user_data = &fw;
  if (coli_edge_select(impl_->edge, &select, error, sizeof(error)) != 0) {
    if (fw.tripped.load()) throw AbiCancelledError("wall-clock firewall tripped during select (prefill)");
    throw AbiError(std::string("select (prefill): ") + error);
  }

  std::vector<std::int32_t> generated;
  generated.push_back(predicted);
  if (resp.ttftMs < 0) {
    resp.ttftMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  }

  // Token-by-token greedy decode with the firewall wired into every call.
  std::vector<float> rowIn(rowBytes / sizeof(float));
  std::vector<float> rowOut(rowBytes / sizeof(float));
  for (int step = 1; step < cfg_.maxTokens; ++step) {
    // F76: EOS stop only when the adapter advertises a real id.
    if (cfg_.stopOnEos && eosTokenId_ >= 0 && predicted == eosTokenId_) break;
    if (fw.tripped.load()) {
      throw AbiCancelledError("wall-clock firewall tripped during decode loop (step " +
                              std::to_string(step) + ")");
    }
    const std::int32_t token = predicted;
    embed.rows = 1;
    embed.token_ids = &token;
    embed.token_count = 1;
    embed.output = rowIn.data();
    embed.output_bytes = rowBytes;
    if (coli_edge_embed(impl_->edge, &embed, error, sizeof(error)) != 0) {
      if (fw.tripped.load()) throw AbiCancelledError("wall-clock firewall tripped during embed (decode)");
      throw AbiError(std::string("embed (decode): ") + error);
    }
    run.rows = 1;
    run.position = static_cast<std::uint64_t>(promptCount) +
                   static_cast<std::uint64_t>(step) - 1;
    run.token_ids = &token;
    run.token_count = 1;
    run.input = rowIn.data();
    run.input_bytes = rowBytes;
    run.output = rowOut.data();
    run.output_bytes = rowBytes;
    if (coli_segment_run(session, &run, error, sizeof(error)) != 0) {
      if (fw.tripped.load()) throw AbiCancelledError("wall-clock firewall tripped during segment_run (decode)");
      throw AbiError(std::string("segment_run (decode): ") + error);
    }
    select.input = rowOut.data();
    if (coli_edge_select(impl_->edge, &select, error, sizeof(error)) != 0) {
      if (fw.tripped.load()) throw AbiCancelledError("wall-clock firewall tripped during select (decode)");
      throw AbiError(std::string("select (decode): ") + error);
    }
    generated.push_back(predicted);
  }

  // Detokenize (sizing then fill). F78: 0 bytes tolerated, never padded.
  size_t textBytes = 0;
  if (coli_edge_detokenize(impl_->edge, generated.data(), generated.size(),
                           nullptr, 0, &textBytes, error,
                           sizeof(error)) != 0) {
    throw AbiError(std::string("detokenize (sizing): ") + error);
  }
  if (textBytes > 0) {
    std::string text(textBytes + 1, '\0');
    if (coli_edge_detokenize(impl_->edge, generated.data(), generated.size(),
                             text.data(), textBytes + 1, &textBytes, error,
                             sizeof(error)) != 0) {
      throw AbiError(std::string("detokenize (fill): ") + error);
    }
    text.resize(textBytes);
    resp.content = std::move(text);
  }

  // Honest in-process usage: these are OUR real token counts (prompt
  // tokenized here, completion decoded here) — not estimates, not
  // engine telemetry. usageEstimated stays false with provenance.
  resp.usage.promptTokens = static_cast<long>(promptCount);
  resp.usage.completionTokens = static_cast<long>(generated.size());
  resp.usage.totalTokens = resp.usage.promptTokens + resp.usage.completionTokens;
  resp.usageEstimated = false;
  resp.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
  // No wire on this lane: lastByte/maxIdle stay -1 (never fabricated).

  // F83: cumulative honest accounting (mirrors LlmClient::postImpl).
  {
    std::lock_guard<std::mutex> l(mu_);
    total_.promptTokens += resp.usage.promptTokens;
    total_.completionTokens += resp.usage.completionTokens;
    total_.totalTokens += resp.usage.totalTokens;
    ++requests_;
  }
  return resp;
}

RouterConfig::AbiFactory makeAbiBackendFactory() {
  return [](const EngineEntry& e) -> std::unique_ptr<ILlmPoster> {
    AbiConfig cfg;
    cfg.modelDir = e.modelDir;
    // F84: adapter family derived from the verbatim model-id —
    // "olmoe-leaf"/"olmoe-colibri" -> "olmoe"; never hardcoded.
    cfg.engineId = deriveFamily(e.modelId);
    cfg.maxTokens = e.maxTokens;
    cfg.timeout = e.timeout;
    cfg.memoryLimitBytes = e.memoryLimitBytes;  // 0 = adapter automatic
    return std::make_unique<AbiClient>(std::move(cfg));
  };
}

}  // namespace dshlite
