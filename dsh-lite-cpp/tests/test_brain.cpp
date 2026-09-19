// test_brain.cpp — Milestone 3 acceptance: history array, judge-first
// gating, fallback escalation (never unverified execution). LLM transport
// is faked via ILlmPoster; no network, no keys.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/compaction.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/nudge.hpp"

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

  // ── Gap 2 integration: turn() compaction + nudge lineage ──────────
  // 6. F28/G2.5: over-budget history is compacted INSIDE turn() before
  // the model sees it. Pre-integration turn() had no compaction at all
  // (history_ grew without bound) — this test fails against old code.
  {
    FakeLlm llm;
    BrainLoop b(llm, [](const std::string&) { return v(JudgeAction::DoDirect); });
    b.setSystemPrompt("sys");
    CompactionConfig cc;
    cc.thresholdChars = 500;
    BrainLoop::HostConfig hc;
    hc.compaction = cc;
    b.setHostConfig(std::move(hc));
    // Fixture law (F28 test): the PROTECTED set (system + last user
    // message) must itself fit the budget, else compaction stops
    // honestly over threshold and this check proves nothing. 300-char
    // turns keep protection ~320 chars < 500.
    for (int i = 0; i < 4; ++i) b.turn(std::string(300, 'x'));
    const auto& h = b.history();
    check(serializedChars(h) <= cc.thresholdChars + 64,
          "G2.5: turn() compacts history before re-injection");
    bool marker = false;
    for (const auto& m : h)
      if (m.content.find(kCompactionMarker) != std::string::npos) marker = true;
    check(marker, "compaction marker present after over-budget turns");
    check(h.front().role == "system" && h.front().content == "sys",
          "F28: system message survived compaction");
    check(h.back().role == "assistant",
          "final assistant reply intact (compaction ran pre-post)");
    // The model never saw an over-budget context:
    check(serializedChars(llm.seen) <= cc.thresholdChars + 64,
          "LLM received the COMPACTED history, not the raw one");
  }

  // 7. F29: user turn resets nudgeDepth but NOT retry rounds.
  {
    FakeLlm llm;
    BrainLoop b(llm, [](const std::string&) { return v(JudgeAction::DoDirect); });
    NudgeState& s = b.nudgeState("t1");
    recordNudge(s, "test");
    recordNudge(s, "test");
    s.rounds = 2;  // simulate exhausted verification retries
    b.turn("hello");  // explicit user turn
    check(s.nudgeDepth == 0, "F29: user turn resets nudgeDepth");
    check(s.rounds == 2, "F29: user turn NEVER launders retry rounds (I6)");
    NudgeState& s2 = b.nudgeState("t1");
    check(&s2 == &s, "nudgeState returns the SAME lineage instance");
  }

  // ── Gap 2 integration: runGatedTask (gate -> spawn -> verify -> caps)
  {
    FakeLlm llm;
    BrainLoop b(llm, [](const std::string&) { return v(JudgeAction::DoDirect); });
    BrainLoop::HostConfig hc;
    hc.policy.allowedTools = {"shell"};
    const std::string ledgerPath = "/tmp/hermes-brain-gate.jsonl";
    ::remove(ledgerPath.c_str());
    LedgerWriter lw(ledgerPath);
    hc.ledger = &lw;
    b.setHostConfig(std::move(hc));

    auto spawnOpts = [](const char* script) {
      SpawnOptions o;
      o.argv = {"/bin/sh", "-c", script};
      o.timeout = std::chrono::seconds(10);
      return o;
    };
    const nlohmann::json okPayload = {{"tool", "shell"}, {"args", {{"cmd", "true"}}}};

    // 8. Clean payload + exit 0 => verified, exactly one spawn.
    auto r1 = b.runGatedTask("g1", okPayload, spawnOpts("exit 0"));
    check(r1.disposition == "verified" && r1.spawns == 1 && r1.verify.pass,
          "G2.4: clean payload spawns once and verifies via exit code");

    // 9. Unknown tool => refused, ZERO spawns (old code had no gate at
    // all — any payload spawned; this fails against pre-gate code).
    // NOTE: args must be a real JSON object — a bare {} in an nlohmann
    // init-list is an ARRAY and would (correctly) trip the schema rule
    // before the allowlist, testing the wrong thing.
    const nlohmann::json bad = {{"tool", "curl"},
                                {"args", {{"cmd", "http://x"}}}};
    auto r2 = b.runGatedTask("g2", bad, spawnOpts("exit 0"));
    check(r2.disposition == "gate-refused" && r2.spawns == 0 &&
              r2.gate.code == "TOOL_NOT_ALLOWED",
          "G2.4: TOOL_NOT_ALLOWED refuses with zero spawns");

    // 10. Destructive verb => propose-only, ZERO spawns.
    const nlohmann::json destr = {{"tool", "shell"},
                                  {"args", {{"cmd", "git push origin"}}}};
    auto r3 = b.runGatedTask("g3", destr, spawnOpts("exit 0"));
    check(r3.disposition == "propose-only" && r3.spawns == 0 &&
              r3.gate.code == "DESTRUCTIVE_PROPOSE_ONLY",
          "G2.4/A.3: destructive verb is propose-only, never auto-spawns");

    // 11. I6 retry cap: always-failing spawn (exit 1), default planner
    // (FullScope) => Allow, Allow, then StopAndReport latched; exactly
    // 3 spawns (initial + 2 retries), never a 4th.
    auto r4 = b.runGatedTask("g4", okPayload, spawnOpts("exit 1"));
    check(r4.disposition == "verify-failed-stop" && r4.spawns == 3,
          "I6: 2 full-scope retries then stop — the 4th spawn never happens");
    check(b.nudgeState("g4").stopped, "lineage latched stopped after cap");

    // 12. Ledger recorded the whole sequence (DENY + verify + nudge +
    // report). LedgerWriter is unbuffered (one write(2) per line), so
    // the file is complete while the writer is still open — no explicit
    // destructor call (that would be UB double-destroy).
    {
      std::ifstream in(ledgerPath);
      std::string line;
      long deny = 0, verifyFail = 0, nudge = 0, reports = 0;
      while (std::getline(in, line)) {
        const auto j = nlohmann::json::parse(line);
        if (j.value("type", "") == "DENY") ++deny;
        if (j.value("type", "") == "verify" && j.value("verdict", "") == "fail")
          ++verifyFail;
        if (j.value("type", "") == "nudge") ++nudge;
        if (j.value("type", "") == "report") ++reports;
        check(j.value("schema", 0) == 2, "integration ledger lines are v2");
      }
      check(deny >= 1 && verifyFail >= 3 && nudge >= 2 && reports >= 2,
            "ledger v2 integration: DENY + verify-fail + nudge + report lines");
    }
    ::remove(ledgerPath.c_str());
  }

  std::cout << (failures == 0 ? "BRAIN PASS\n" : "BRAIN FAIL\n");
  return failures == 0 ? 0 : 1;
}
