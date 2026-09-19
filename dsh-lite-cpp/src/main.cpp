// main.cpp — dsh-lite demo: Brain turn against a local `coli serve`
// engine, or offline path (sanitizer + spawner + judge-gate stub).
//
// Usage:
//   coli serve --model <weights> --model-id glm-5.3-flash-colibri &
//   dsh-lite "your strategic question"   # live Brain turn + judge gate
//   dsh-lite --offline                   # no engine: spawner + gate demo

#include <cstdlib>
#include <iostream>
#include <string>

#include "dshlite/brain.hpp"
#include "dshlite/llm_client.hpp"
#include "dshlite/sanitizer.hpp"
#include "dshlite/spawner.hpp"

namespace {

struct EchoLlm : dshlite::ILlmPoster {
  dshlite::LlmResponse post(const std::vector<dshlite::Message>& m) override {
    return dshlite::LlmResponse{"(offline) strategy noted for: " +
                                    (m.empty() ? "" : m.back().content),
                                {}, 0};
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string arg = argc > 1 ? argv[1] : "";
  dshlite::LlmConfig probe;
  const bool engineWanted = (arg != "--offline");
  if (arg == "--offline") {
    std::cout << "(offline demo requested: no engine traffic)\n";
  } else if (arg.empty()) {
    arg = "--offline";
    std::cout << "(no question given: running offline demo)\n";
  }
  if (arg == "--offline") {
    // Spawner demo: hostile output sanitized before the Brain sees it.
    dshlite::SwarmSpawner sp;
    dshlite::SpawnOptions o;
    o.argv = {"printf", "\\033[32mworker done\\033[0m \\001\\n"};
    dshlite::SpawnResult r = sp.spawn(o);
    std::cout << "worker exit=" << r.exitCode
              << " brain-sees=[" << r.sanitizedStdout << "]\n";
    dshlite::SwarmSpawner::cleanup(r.workspaceDir);

    // Judge gate demo with a stubbed Brain (no network).
    EchoLlm llm;
    dshlite::BrainLoop brain(
        llm, [](const std::string& c) -> dshlite::JudgeVerdict {
          dshlite::JudgeVerdict v;
          v.route = "do_direct";
          v.confidence = 0.9;
          v.action = dshlite::JudgeAction::DoDirect;
          v.detail = "stub judge: atomic (" + c + ")";
          return v;
        });
    brain.setSystemPrompt("You are the executive Brain. Never execute labor.");
    std::cout << brain.turn("draft the milestone plan") << "\n";
    dshlite::JudgeVerdict g = brain.gateDelegation("write the changelog");
    std::cout << "gate: action=" << static_cast<int>(g.action)
              << " route=" << g.route << " conf=" << g.confidence << "\n";
    std::cout << "OFFLINE DEMO PASS\n";
    return 0;
  }

  // Live path: Brain + local colibri engine + node judge hook.
  // No API key needed on loopback; COLI_API_KEY only for remote binds.
  (void)engineWanted;
  (void)probe;
  try {
    dshlite::LlmConfig cfg;  // defaults: 127.0.0.1:8000 + engine model-id
    if (const char* m = std::getenv("COLI_MODEL_ID")) cfg.model = m;
    dshlite::LlmClient llm(cfg);
    dshlite::BrainLoop brain(llm, dshlite::makeNodeJudgeHook());
    brain.setSystemPrompt(
        "You are the executive Brain of a Budget-AGI harness. Keep "
        "high-level strategy only; delegate labor via the judge gate.");
    std::cout << brain.turn(arg) << "\n";
    dshlite::JudgeVerdict g = brain.gateDelegation(arg);
    std::cout << "gate: action=" << static_cast<int>(g.action)
              << " route=" << g.route << " conf=" << g.confidence
              << (g.fallback ? " (fallback: ask the user)" : "") << "\n";
    const auto t = llm.totalUsage();
    std::cout << "tokens: prompt=" << t.promptTokens
              << " completion=" << t.completionTokens
              << " total=" << t.totalTokens
              << " requests=" << llm.requestCount() << "\n";
  } catch (const std::exception& e) {
    std::cerr << "dsh-lite: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
