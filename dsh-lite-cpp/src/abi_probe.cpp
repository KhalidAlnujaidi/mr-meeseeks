// abi_probe.cpp — direct C ABI feasibility spike against Colibri's
// segment/edge runtime (libcolibri_segment_edge.a). NO `coli serve`, NO
// Python, NO HTTP: the whole decode happens in-process through the
// public C ABI.
//
// Pipeline per generated token:
//   tokenize -> embed (edge) -> segment_run (all layers) -> select (edge)
//   -> detokenize (edge)
//
// Usage: abi-probe <model-dir> [max-new-tokens] [prompt...]
//   default max-new = 24, default prompt "The capital of France is"
//
// Exit codes: 0 success (text printed), 2 usage/ABI failure (stderr).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "edge_adapters.h"
#include "edge_runtime.h"
#include "segment_adapters.h"
#include "segment_runtime.h"
}

namespace {

void die(const char* where, const char* error) {
  std::fprintf(stderr, "abi-probe: %s failed: %s\n", where,
               error[0] ? error : "(no message)");
}

// 8 GiB explicit budget for the segment engine — set directly from C++,
// proving opts.memory_limit_bytes is honored across the C ABI (0 would
// mean "adapter automatic"; we deliberately do NOT rely on that).
constexpr std::uint64_t kMemoryLimitBytes = 8ull * 1024 * 1024 * 1024;

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: abi-probe <model-dir> [max-new-tokens] [prompt...]\n");
    return 2;
  }
  const std::string modelDir = argv[1];
  const int maxNew = argc >= 3 ? std::atoi(argv[2]) : 24;
  std::string prompt = "The capital of France is";
  if (argc > 3) {  // remaining args form the prompt verbatim
    prompt.clear();
    for (int i = 3; i < argc; ++i) {
      if (i > 3) prompt += " ";
      prompt += argv[i];
    }
  }
  if (maxNew < 1) return 2;

  char error[512] = {0};

  // 1. Explicit adapter registration (ordinary CLI/serve never do this;
  //    the registry is write-once at init, then read-only).
  if (coli_olmoe_segment_adapter_register() != 0) {
    std::fprintf(stderr, "abi-probe: olmoe segment adapter register failed\n");
    return 2;
  }
  if (coli_olmoe_edge_adapter_register() != 0) {
    std::fprintf(stderr, "abi-probe: olmoe edge adapter register failed\n");
    return 2;
  }
  std::printf("[abi] adapters registered: segment=%d edge=%d\n",
              coli_segment_adapter_count(), coli_edge_adapter_count());

  // 2. Open the EDGE side (tokenizer + embedding + final head) with an
  //    explicit memory_limit_bytes set from C++.
  ColiEdgeEngineOptions edgeOpts{};
  edgeOpts.struct_size = sizeof(edgeOpts);
  edgeOpts.model_dir = modelDir.c_str();
  edgeOpts.memory_limit_bytes = kMemoryLimitBytes;
  ColiEdgeEngine* edge = nullptr;
  if (coli_edge_engine_open("olmoe", &edgeOpts, &edge, error,
                            sizeof(error)) != 0) {
    die("coli_edge_engine_open", error);
    return 2;
  }
  ColiEdgeCapabilities edgeCap{};
  edgeCap.struct_size = sizeof(edgeCap);
  if (coli_edge_engine_capabilities(edge, &edgeCap, error, sizeof(error)) != 0) {
    die("coli_edge_engine_capabilities", error);
    coli_edge_engine_close(edge);
    return 2;
  }
  std::printf("[abi] edge open: engine_id=%s vocab=%u layers=%u width=%u "
              "dtype=%u max_ctx=%u eos=%d mem_limit=%llu\n",
              edgeCap.engine_id, edgeCap.vocab_size, edgeCap.num_layers,
              edgeCap.state_width, edgeCap.state_dtype,
              edgeCap.max_context_tokens, edgeCap.eos_token_id,
              static_cast<unsigned long long>(kMemoryLimitBytes));
  if (!(edgeCap.flags & COLI_EDGE_CAP_TOKENIZE) ||
      !(edgeCap.flags & COLI_EDGE_CAP_GREEDY)) {
    std::fprintf(stderr, "abi-probe: adapter lacks tokenize/greedy caps\n");
    coli_edge_engine_close(edge);
    return 2;
  }

  // 3. Open the SEGMENT side: ALL layers [0, num_layers), explicit memory
  //    budget, context sized to prompt + generation.
  //    (Tokenize first so we know the prompt length.)
  size_t promptCount = 0;
  if (coli_edge_tokenize(edge, prompt.c_str(), prompt.size(), nullptr, 0,
                         &promptCount, error, sizeof(error)) != 0 ||
      promptCount == 0) {
    die("coli_edge_tokenize (sizing)", error);
    coli_edge_engine_close(edge);
    return 2;
  }
  std::vector<std::int32_t> promptIds(promptCount);
  if (coli_edge_tokenize(edge, prompt.c_str(), prompt.size(), promptIds.data(),
                         promptCount, &promptCount, error,
                         sizeof(error)) != 0) {
    die("coli_edge_tokenize (fill)", error);
    coli_edge_engine_close(edge);
    return 2;
  }
  std::printf("[abi] tokenized %zu prompt tokens\n", promptCount);

  const std::uint32_t context =
      static_cast<std::uint32_t>(promptCount) + static_cast<std::uint32_t>(maxNew) + 2;
  ColiSegmentEngineOptions segOpts{};
  segOpts.struct_size = sizeof(segOpts);
  segOpts.model_dir = modelDir.c_str();
  segOpts.layer_begin = 0;
  segOpts.layer_end = edgeCap.num_layers;
  segOpts.context_tokens = context;
  segOpts.memory_limit_bytes = kMemoryLimitBytes;
  ColiSegmentEngine* segment = nullptr;
  if (coli_segment_engine_open("olmoe", &segOpts, &segment, error,
                               sizeof(error)) != 0) {
    die("coli_segment_engine_open", error);
    coli_edge_engine_close(edge);
    return 2;
  }
  ColiSegmentSessionOptions sessOpts{};
  sessOpts.struct_size = sizeof(sessOpts);
  sessOpts.context_tokens = context;
  ColiSegmentSession* session = nullptr;
  if (coli_segment_session_create(segment, &sessOpts, &session, error,
                                  sizeof(error)) != 0) {
    die("coli_segment_session_create", error);
    (void)coli_segment_engine_close(segment, nullptr, 0);
    coli_edge_engine_close(edge);
    return 2;
  }
  std::printf("[abi] segment open: layers=[0,%u) ctx=%u — in-process, no serve\n",
              edgeCap.num_layers, context);

  // 4. Prefill: embed prompt rows, run the whole layer stack, select the
  //    next token from the LAST prompt row's output activation.
  const size_t rowBytes = static_cast<size_t>(edgeCap.state_width) * sizeof(float);
  std::vector<float> in(promptCount * rowBytes / sizeof(float));
  std::vector<float> out(promptCount * rowBytes / sizeof(float));

  ColiEdgeEmbedRequest embed{};
  embed.struct_size = sizeof(embed);
  embed.rows = static_cast<std::uint32_t>(promptCount);
  embed.token_ids = promptIds.data();
  embed.token_count = promptCount;
  embed.output = in.data();
  embed.output_bytes = in.size() * sizeof(float);
  if (coli_edge_embed(edge, &embed, error, sizeof(error)) != 0) {
    die("coli_edge_embed (prefill)", error);
    return 2;
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
  if (coli_segment_run(session, &run, error, sizeof(error)) != 0) {
    die("coli_segment_run (prefill)", error);
    return 2;
  }

  std::int32_t predicted = -1;
  float score = 0.0f;
  ColiEdgeSelectRequest select{};
  select.struct_size = sizeof(select);
  select.rows = 1;
  select.input = out.data() + (promptCount - 1) * edgeCap.state_width;
  select.input_bytes = rowBytes;
  select.token_ids = &predicted;
  select.token_capacity = 1;
  select.scores = &score;
  select.score_capacity = 1;
  if (coli_edge_select(edge, &select, error, sizeof(error)) != 0) {
    die("coli_edge_select (prefill)", error);
    return 2;
  }

  // 5. Token-by-token greedy decode loop.
  std::vector<std::int32_t> generated;
  generated.push_back(predicted);
  std::vector<float> rowIn(rowBytes / sizeof(float));
  std::vector<float> rowOut(rowBytes / sizeof(float));
  for (int step = 1; step < maxNew; ++step) {
    if (edgeCap.eos_token_id >= 0 && predicted == edgeCap.eos_token_id) break;
    const std::int32_t token = predicted;
    embed.rows = 1;
    embed.token_ids = &token;
    embed.token_count = 1;
    embed.output = rowIn.data();
    embed.output_bytes = rowBytes;
    if (coli_edge_embed(edge, &embed, error, sizeof(error)) != 0) {
      die("coli_edge_embed (decode)", error);
      return 2;
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
      die("coli_segment_run (decode)", error);
      return 2;
    }
    select.input = rowOut.data();
    if (coli_edge_select(edge, &select, error, sizeof(error)) != 0) {
      die("coli_edge_select (decode)", error);
      return 2;
    }
    generated.push_back(predicted);
  }

  // 6. Detokenize generated ids (sizing pass then fill — ABI contract).
  size_t textBytes = 0;
  if (coli_edge_detokenize(edge, generated.data(), generated.size(), nullptr,
                           0, &textBytes, error, sizeof(error)) != 0) {
    die("coli_edge_detokenize (sizing)", error);
    return 2;
  }
  std::string text(textBytes + 1, '\0');
  if (coli_edge_detokenize(edge, generated.data(), generated.size(), text.data(),
                           textBytes + 1, &textBytes, error,
                           sizeof(error)) != 0) {
    die("coli_edge_detokenize (fill)", error);
    return 2;
  }
  text.resize(textBytes);

  std::printf("\n=== RAW GENERATED TEXT (%zu tokens, greedy, in-process) ===\n%s\n"
              "=== END RAW ===\n",
              generated.size(), text.c_str());

  coli_segment_session_destroy(session);
  if (coli_segment_engine_close(segment, error, sizeof(error)) != 0) {
    die("coli_segment_engine_close", error);
  }
  coli_edge_engine_close(edge);
  std::printf("[abi] clean shutdown: session destroyed, engines closed\n");
  return 0;
}
