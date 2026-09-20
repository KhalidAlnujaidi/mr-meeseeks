// abi_bench.cpp — in-process C ABI vs HTTP coli-serve side-by-side bench.
// Runs the SAME multi-turn reasoning prompt through both lanes with the
// same maxTokens, SEQUENTIALLY (F70 fairness: single model on one disk-
// bound box; concurrent runs would contend for page cache and skew both
// arms). Metrics per arm: ttft_ms, decode tok/s (F13 window), total
// wall seconds — from the same LlmResponse struct on both lanes, so the
// numbers are structurally comparable.
//
// Usage:
//   abi-bench abi  <model-dir> <max-tokens> <repeats>
//   abi-bench http <engine-url> <model-id> <max-tokens> <repeats>
//
// Zero-IPC speedup = http_wall / abi_wall on identical prompts; the
// caller runs both arms and diffs the JSON lines this prints.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "dshlite/abi_client.hpp"
#include "dshlite/llm_client.hpp"

namespace {

// A standard multi-turn reasoning prompt (same conversation both arms).
std::vector<dshlite::Message> promptTurns() {
  return {
      {"system", "You are a careful reasoning assistant. Answer concisely."},
      {"user", "A bat and a ball cost 110 cents in total. The bat costs "
               "100 cents more than the ball. How many cents does the "
               "ball cost? Think step by step, then give the number."},
  };
}

void emit(const std::string& arm, int rep, const dshlite::LlmResponse& r) {
  const double tps = dshlite::decodeTokPerSec(r);
  // Minimal JSON string escaping so bench lines always parse.
  auto esc = [](const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (const char c : s) {
      switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
          } else {
            o += c;
          }
      }
    }
    return o;
  };
  std::cout << "{\"arm\":\"" << arm << "\",\"rep\":" << rep
            << ",\"ttft_ms\":" << r.ttftMs
            << ",\"latency_ms\":" << r.latencyMs
            << ",\"completion_tokens\":" << r.usage.completionTokens
            << ",\"prompt_tokens\":" << r.usage.promptTokens
            << ",\"tok_per_sec\":" << (tps > 0 ? std::to_string(tps) : "null")
            << ",\"usage_estimated\":" << (r.usageEstimated ? "true" : "false")
            << ",\"content_head\":\""
            << esc(r.content.substr(0, std::min<size_t>(60, r.content.size())))
            << "\"}\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dshlite;
  if (argc < 5) {
    std::cerr << "usage: abi-bench abi <model-dir> <max-tokens> <repeats>\n"
                 "       abi-bench http <engine-url> <model-id> <max-tokens> <repeats>\n";
    return 2;
  }
  const std::string arm = argv[1];
  const int maxTokens = std::atoi(argv[argc - 2]);
  const int repeats = std::atoi(argv[argc - 1]);
  if (maxTokens < 1 || repeats < 1) return 2;

  const auto msgs = promptTurns();
  if (arm == "abi") {
    AbiConfig cfg;
    cfg.modelDir = argv[2];
    cfg.maxTokens = maxTokens;
    cfg.timeout = std::chrono::seconds(900);
    AbiClient client(cfg);
    for (int i = 0; i < repeats; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      auto r = client.post(msgs);
      r.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
      emit("abi", i, r);
    }
  } else if (arm == "http") {
    LlmConfig cfg;
    cfg.endpoint = argv[2];
    cfg.model = argv[3];
    cfg.maxTokens = maxTokens;
    cfg.timeout = std::chrono::seconds(900);
    cfg.stream = true;  // TTFT parity: both arms measure first token
    cfg.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
    LlmClient client(cfg);
    for (int i = 0; i < repeats; ++i) emit("http", i, client.post(msgs));
  } else {
    std::cerr << "unknown arm: " << arm << "\n";
    return 2;
  }
  return 0;
}
