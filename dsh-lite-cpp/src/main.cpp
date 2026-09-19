// main.cpp — dsh-lite demo: Brain turn (needs OPENROUTER_API_KEY) or
// offline path (sanitizer + spawner + judge-gate demo with a stubbed LLM).
//
// Usage:
//   dsh-lite "your strategic question"   # live Brain turn + judge gate
//   dsh-lite --offline                   # no network: spawner + gate demo

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
  if (arg == "--offline" || std::getenv("OPENROUTER_API_KEY") == nullptr) {
    if (arg != "--offline")
      std::cout << "(no OPENROUTER_API_KEY: running offline demo)\n";
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

  // Live path: Brain + OpenRouter + node judge hook.
  try {
    dshlite::LlmConfig cfg;
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
