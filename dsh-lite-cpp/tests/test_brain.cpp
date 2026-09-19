// test_brain.cpp — Milestone 3 acceptance: history array, judge-first
// gating, fallback escalation (never unverified execution). LLM transport
// is faked via ILlmPoster; no network, no keys.

#include <iostream>
#include <string>
#include <vector>

#include "dshlite/brain.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

// Fake LLM: records messages, echoes canned text with zero token usage.
struct FakeLlm : dshlite::ILlmPoster {
  std::string canned = "strategy: split once";
  std::vector<dshlite::Message> seen;
  dshlite::LlmResponse post(const std::vector<dshlite::Message>& m) override {
    seen = m;
    return dshlite::LlmResponse{canned, {}, 1};
  }
};

dshlite::JudgeVerdict v(dshlite::JudgeAction a, bool fb = false) {
  dshlite::JudgeVerdict j;
  j.action = a;
  j.route = "do_direct";
  j.confidence = 0.95;
  j.fallback = fb;
  j.detail = "test";
  return j;
}
}  // namespace

int main() {
  using namespace dshlite;

  // 1. History array: system + user/assistant pairs accumulate.
  {
    FakeLlm llm;
    BrainLoop b(llm, [](const std::string&) { return v(JudgeAction::DoDirect); });
    b.setSystemPrompt("you are the executive brain");
    check(b.turn("plan the release") == "strategy: split once", "turn returns text");
    const auto& h = b.history();
    check(h.size() == 3 && h[0].role == "system" && h[1].role == "user" &&
              h[2].role == "assistant",
          "history = system/user/assistant array");
    check(llm.seen.size() == 2, "LLM received system + user");
  }

  // 2. Judge hook runs BEFORE delegation (spy ordering).
  {
    FakeLlm llm;
    bool judged = false;
    BrainLoop b(llm, [&](const std::string& c) {
      judged = true;
      check(c == "write docs", "judge sees criterion text");
      return v(JudgeAction::SplitOnce);
    });
    JudgeVerdict r = b.gateDelegation("write docs");
    check(judged, "judge hook invoked first");
    check(r.action == JudgeAction::SplitOnce, "split verdict passes through");
  }

  // 3. Fallback degradation: judge timeout => escalate, never execute.
  {
    FakeLlm llm;
    BrainLoop b(llm, [](const std::string&) -> JudgeVerdict {
      throw std::runtime_error("judge: timeout after 30s");
    });
    JudgeVerdict r = b.gateDelegation("migrate prod db");
    check(r.action == JudgeAction::Escalate && r.fallback,
          "judge failure escalates to user");
    check(r.detail.find("ESCALAT") != std::string::npos,
          "escalation message names the user path");
  }

  // 4. Fallback-flagged verdict forced to escalate even if action disagrees.
  {
    FakeLlm llm;
    BrainLoop b(llm,
                [](const std::string&) { return v(JudgeAction::DoDirect, true); });
    JudgeVerdict r = b.gateDelegation("push to prod");
    check(r.action == JudgeAction::Escalate, "fallback forces escalate");
  }

  // 5. SpiralStop passes through with guidance.
  {
    FakeLlm llm;
    BrainLoop b(llm, [](const std::string&) {
      JudgeVerdict j = v(JudgeAction::SpiralStop);
      j.detail = "";
      return j;
    });
    JudgeVerdict r = b.gateDelegation("same task again");
    check(r.action == JudgeAction::SpiralStop && !r.detail.empty(),
          "spiral stop carries guidance, no lockup");
  }

  std::cout << (failures == 0 ? "BRAIN PASS\n" : "BRAIN FAIL\n");
  return failures == 0 ? 0 : 1;
}
