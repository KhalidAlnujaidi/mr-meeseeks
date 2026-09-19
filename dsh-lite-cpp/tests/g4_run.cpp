// g4-run.cpp — Gap 4 driver: live uncapped agent turns against a local
// `coli serve` engine through the FULL host-enforced C++ stack:
//   ModelRouter (role->endpoint/model-id, fallback, velocity floor)
//   + BrainLoop (compaction F28, nudge lineages F29)
//   + runGatedTask (payload gate -> spawn -> exit-code verify -> I6/I7)
//   + LedgerWriter v2 (socket-boundary tok/s, ttft, DENY/nudge/verify).
//
// Zero external network (G4.2): loopback engine only; remote-judge keys
// asserted absent (F9). Ledger path: /tmp/g4-ledger.jsonl (bench law:
// never prod state).
//
// Usage: g4-run <brain-url> <brain-model-id> <leaf-url> <leaf-model-id> [turns]
// F2 law: the brain endpoint may never sit in a leaf pool, so a valid
// run needs two distinct endpoints (one engine may serve twice on two
// ports — same family trips the G1.2 warning, which this driver
// surfaces honestly rather than hiding).
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/router.hpp"

namespace {
// Router-backed poster so BrainLoop::turn() traffic flows through the
// brain pool (velocity floor + attempt log included). Emits one v2
// `report` ledger line per brain turn (G4.1 execution trace) via
// fromRouted — latency/ttft/usage captured at the socket boundary.
struct RouterPoster : dshlite::ILlmPoster {
  dshlite::ModelRouter& router;
  dshlite::LedgerWriter* ledger = nullptr;
  long turnNo = 0;
  explicit RouterPoster(dshlite::ModelRouter& r) : router(r) {}
  dshlite::LlmResponse post(const std::vector<dshlite::Message>& m) override {
    auto rr = router.post(dshlite::Role::Brain, m);
    if (ledger) {
      ++turnNo;
      auto ev = dshlite::LedgerWriter::fromRouted(
          "report", "brain", "g4.brain-turn" + std::to_string(turnNo), 0, rr);
      // F33 finding: colibri SSE streams carry NO usage chunk (verified
      // by raw curl) — streamed turns honestly record 0 tokens (F12),
      // never invented. Non-streaming calls DO return usage.
      if (!rr.attempts.empty())
        ev.detail = "attempts=" + std::to_string(rr.attempts.size()) +
                    " last=" + rr.attempts.back().outcome;
      ledger->append(ev);
    }
    return rr.response;
  }
};
}  // namespace

int main(int argc, char** argv) {
  using namespace dshlite;
  if (argc < 5) {
    std::cerr << "usage: g4-run <brain-url> <brain-model-id> "
                 "<leaf-url> <leaf-model-id> [turns]\n";
    return 2;
  }
  const std::string brainUrl = argv[1];
  const std::string brainModel = argv[2];
  const std::string leafUrl = argv[3];
  const std::string leafModel = argv[4];
  const int turns = argc > 5 ? std::atoi(argv[5]) : 3;

  // G4.2/F9: zero-external-network law — remote judge keys must be absent.
  try {
    assertNoRemoteJudgeEnv();
  } catch (const std::exception& e) {
    std::cerr << "g4-run REFUSED (zero-network law): " << e.what() << "\n";
    return 3;
  }

  LedgerWriter ledger("/tmp/g4-ledger.jsonl");

  auto makeEntry = [](const std::string& url, const std::string& model) {
    EngineEntry e;
    e.endpoint = url;
    e.modelId = model;
    e.maxTokens = 256;
    e.timeout = std::chrono::seconds(600);  // disk-bound, outer watchdog
    e.stream = true;                        // ttft + tok/s at socket boundary
    e.minTokPerSec = 0.02;  // below OLMoE's honest floor would be thrashing
    e.warmupTurns = 2;      // F4: cold NVMe ramp exempt
    return e;
  };

  RouterConfig rc;
  rc.brain = {makeEntry(brainUrl, brainModel)};
  rc.worker = {makeEntry(leafUrl, leafModel)};     // F2: disjoint endpoint
  rc.verifier = {makeEntry(leafUrl, leafModel)};
  ModelRouter router(rc);
  for (const auto& w : router.warnings())
    std::cout << "[warn] " << w << "\n";

  // Pre-flight probe (G1.1): engine must answer its own model-id.
  for (const auto& p : router.probe()) {
    std::cout << "[probe] " << p.modelId << " @ " << p.endpoint << " -> "
              << (p.ok ? "OK" : "FAIL: " + p.detail) << "\n";
    if (!p.ok) return 4;
  }

  RouterPoster poster(router);
  poster.ledger = &ledger;
  BrainLoop brain(poster, [](const std::string& c) -> JudgeVerdict {
    // Local atomicity heuristic (no remote judge, F9): one-liner criteria
    // are atomic; anything compound splits once. The C++ lane's real
    // judge integration lands with grammar-forced payloads (G2.4 note).
    JudgeVerdict v;
    v.route = "do_direct";
    v.confidence = 0.9;
    v.action = c.size() < 80 ? JudgeAction::DoDirect : JudgeAction::SplitOnce;
    v.detail = "local heuristic judge";
    return v;
  });
  BrainLoop::HostConfig hc;
  hc.policy.allowedTools = {"shell"};
  hc.ledger = &ledger;
  brain.setHostConfig(std::move(hc));
  brain.setSystemPrompt(
      "You are the Budget-AGI brain. Answer in one or two sentences.");

  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < turns; ++i) {
    const std::string q =
        "Turn " + std::to_string(i + 1) +
        ": In one sentence, why does atomic task decomposition help small models?";
    std::cout << "\n[user] " << q << "\n";
    const auto tt = std::chrono::steady_clock::now();
    const std::string a = brain.turn(q);
    const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tt)
                        .count();
    std::cout << "[brain " << ms << " ms] " << a << "\n";

    // One gated task per turn: a trivially verifiable atom (exit 0),
    // exercising gate -> spawn -> verify -> ledger v2 end to end.
    SpawnOptions opt;
    opt.argv = {"/bin/sh", "-c", "echo atom-done; exit 0"};
    opt.timeout = std::chrono::seconds(30);
    const nlohmann::json payload = {{"tool", "shell"},
                                    {"args", {{"cmd", "echo atom-done"}}}};
    auto rep = brain.runGatedTask("g4.t" + std::to_string(i + 1), payload, opt);
    std::cout << "[task g4.t" << i + 1 << "] disposition=" << rep.disposition
              << " spawns=" << rep.spawns
              << " verify=" << (rep.verify.pass ? "pass" : "fail") << "\n";

    // Depth distribution datapoint (VISION.md instrument): uncapped here
    // because the gate loop terminates on verify-pass; caps bind on failure.
  }
  const long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();

  const auto usage = router.totalUsage();
  std::cout << "\n=== G4 run summary ===\n"
            << "turns=" << turns << " wall_ms=" << totalMs
            << " requests=" << router.requestCount()
            << " prompt_tokens=" << usage.promptTokens
            << " completion_tokens=" << usage.completionTokens << "\n";
  std::cout << "history_messages=" << brain.history().size()
            << " serialized_chars=" << serializedChars(brain.history()) << "\n";
  std::cout << "ledger: /tmp/g4-ledger.jsonl\n";
  return 0;
}
