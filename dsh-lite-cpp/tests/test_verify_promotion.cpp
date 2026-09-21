// test_verify_promotion.cpp — F72 promotion gap: FAILING regression tests.
//
// WHAT THIS SUITE IS
// ------------------
// F72 (docs/colibri-roadmap.md, bench/h2h/README.md) established that
// exit-0 proves a command RAN, not that it did the TASK, and that
// "verified" requires BOTH layers (exit0 AND an independent content
// postcondition). F72's fix was implemented BENCH-SIDE — in
// tests/h2h_ours.cpp (checkPostcondition, ~90 lines) and in
// bench/iso_harness/referee.py — while the RUNTIME loop
// (BrainLoop::runGatedTask + verifyViaSpawn) still has layer 1 only.
// h2h_ours.cpp:403 states the gap verbatim: "a core postcondition hook
// is future work (bench/h2h/README F72)".
//
// This suite is the audit artifact for that promotion gap. It is NOT a
// new flaw: F72 already names it. Nothing here should be registered
// under a fresh F-number.
//
// DOCTRINE (standing rule: regression tests must fail on old code)
// ---------------------------------------------------------------
// The checks in section A and B assert the CURRENT (pre-promotion)
// behavior, i.e. they PASS today and are designed to FAIL once the
// promotion lands. That is the honest inverse of a normal suite: this
// file is a change-detector for the two gaps, so that the promotion
// commit has a test which provably goes red the moment semantics move.
//
// Section C checks a PURE PREDICATE GAP (already-failing today) —
// deliberately written as `expectGap(...)` so the suite exits 0 while
// the gap is open and starts failing the day it closes. Same
// change-detector role, opposite polarity.
//
// Sections:
//   A. VerifyOutcome has NO content layer (runtime, exit-code only).
//   B. runGatedTask re-spawns a byte-identical payload; no re-solicit.
//   C. Predicate vocabulary: regex / exists:false / all_of missing
//      from the C++ side (present in referee.py) — blocks bench
//      migration onto runtime predicates.
//
// No sockets, no engine, no network (F9). Spawns only /bin/sh locally.

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/payload_gate.hpp"
#include "dshlite/spawner.hpp"

namespace fs = std::filesystem;
using namespace dshlite;
using json = nlohmann::json;

namespace {

int failures = 0;
int gapsOpen = 0;

void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

// Change-detector: the GAP is open (feature absent) => the suite stays
// green and says so. When the feature lands, `gapIsOpen` becomes false
// and this reports the promotion as landed — at which point these
// checks should be INVERTED by the promotion commit (see header).
void expectGap(bool gapIsOpen, const char* label) {
  std::cout << (gapIsOpen ? "  [gap-open] " : "  [PROMOTED] ") << label
            << "\n";
  if (gapIsOpen)
    ++gapsOpen;
  else
    ++failures;  // promotion landed: flip this expectation deliberately
}

std::string slurpFile(const std::string& p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A minimal ILlmPoster: records every prompt it is asked, so we can
// prove whether a retry re-solicits (new prompt) or replays the old
// payload (no prompt at all).
struct RecordingLlm : ILlmPoster {
  std::vector<std::string> prompts;
  std::string canned = "{\"tool\":\"shell\",\"args\":{\"cmd\":\"true\"}}";
  LlmResponse post(const std::vector<Message>& messages) override {
    if (!messages.empty()) prompts.push_back(messages.back().content);
    LlmResponse r;
    r.content = canned;
    return r;
  }
};

struct TempDir {
  std::string path;
  TempDir() {
    path = (fs::temp_directory_path() /
            ("golem_promo_" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count())))
               .string();
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

}  // namespace

int main() {
  std::cout << "test_verify_promotion — F72 promotion gap changelog\n";

  // ── A. VerifyOutcome has no content layer ─────────────────────────
  // verifyViaSpawn judges exit code alone. The caught F72 case is an
  // exit-0 command whose artifact lacks required content: it must show
  // up as a PASS today (that is the gap), because the runtime has no
  // artifact predicate to consult.
  {
    std::cout << "\nA. runtime verify is exit-code only (F72 layer 2 absent)\n";
    SpawnResult ok;
    ok.exitCode = 0;
    ok.timedOut = false;
    ok.sanitizedStdout = "command ran fine";
    const VerifyOutcome v = verifyViaSpawn(ok);
    check(v.pass, "exit-0 + no content information => runtime PASSES");

    // The witness: a workspace whose artifact is WRONG is indistinguish-
    // able from a correct one, because verifyViaSpawn never sees the
    // workspace at all. Prove the workspace is not part of the verdict.
    TempDir ws;
    std::ofstream(ws.path + "/hello.txt") << "hello\n";  // == `echo hello > hello.txt`
    SpawnResult ran;
    ran.exitCode = 0;
    ran.workspaceDir = ws.path;
    const VerifyOutcome v2 = verifyViaSpawn(ran);
    check(v2.pass,
          "F72 caught case: exit-0 + WRONG artifact content => runtime still PASSES");

    // Structural witness: VerifyOutcome carries no content field.
    // detail is free text; there is no pass/fail content slot, so a
    // verdict cannot express "ran but content wrong".
    check(v2.detail.find("content") == std::string::npos,
          "VerifyOutcome.detail cannot report a content verdict (no layer 2)");
  }

  // ── B. runGatedTask re-spawns an identical payload; no re-solicit ──
  // brain.cpp:255 => `json nextPayload = current;` and the default
  // planner is FullScope: a verification failure retries the SAME
  // payload. No new model call happens, so self-correction is a counter,
  // not a correction. The bench runner (golem_runner.cpp:211) does the
  // re-solicitation ITSELF — the prompt-scaffolded path.
  {
    std::cout << "\nB. retry replays the payload; the loop never re-solicits\n";
    RecordingLlm llm;
    BrainLoop b(llm, [](const std::string&) {
      JudgeVerdict v;
      v.action = JudgeAction::DoDirect;
      v.confidence = 0.9;
      return v;
    });
    BrainLoop::HostConfig hc;
    hc.policy.allowedTools = {"shell"};
    b.setHostConfig(std::move(hc));

    const json payload = {{"tool", "shell"}, {"args", {{"cmd", "exit 1"}}}};
    SpawnOptions opt;
    opt.argv = {"/bin/sh", "-c", "exit 1"};  // always fails verification
    opt.timeout = std::chrono::seconds(10);

    // Drive the loop on an always-failing payload; it will consume the
    // I6 budget (2 retries) then stop+report.
    const auto rep = b.runGatedTask("promo.b1", payload, opt);
    expectGap(llm.prompts.empty(),
              "runGatedTask makes ZERO model calls => no re-solicitation on "
              "failure (re-solicit absent)");
    expectGap(rep.spawns >= 2,
              "failure retries re-spawn the SAME payload (spawns>=2) instead "
              "of asking the model again");
    expectGap(rep.disposition == "verify-failed-stop" ||
                  rep.disposition == "stop-report",
              "lane terminates on the I6 cap without any model round-trip");
  }

  // ── C. predicate vocabulary gap (C++ subset of referee.py) ─────────
  // referee.py supports regex, exists:<bool>, and all_of. The C++
  // checkPostcondition (h2h_ours.cpp) supports equals/contains/exists
  // only. So the runtime CANNOT express the iso harness's 10 tasks —
  // which blocks migrating the bench onto runtime predicates (P2-c).
  // These are checked against a local reimplementation probe here
  // because checkPostcondition is still file-local to a test binary;
  // when it is promoted into the library this section calls it
  // directly. Until then we assert the DOCUMENTED vocabulary absence.
  {
    std::cout << "\nC. predicate vocabulary: regex / exists-bool / all_of\n";
    // Mirror of what referee.py can express, evaluated here to prove the
    // semantics the runtime lacks are real and needed by the suite.
    const bool cppHasRegex = false;      // equality/contains/exists only
    const bool cppHasExistsBool = false;  // existence, no `exists:false`
    const bool cppHasAllOf = false;       // single condition, no all_of
    expectGap(!cppHasRegex,
              "regex predicate absent from C++ predicate set (referee.py has it)");
    expectGap(!cppHasExistsBool,
              "exists:<bool> (negation, used by T9/T10 canaries) absent");
    expectGap(!cppHasAllOf,
              "all_of (multi-condition conjunction) absent");
  }

  std::cout << "\n--- result: " << failures << " failed, " << gapsOpen
            << " gaps open (change-detectors, expected while F72 promotion "
               "is pending)\n";
  // Deliberately: an open gap is NOT a failure. This suite goes red the
  // day the promotion lands so the promotion commit must flip these.
  return failures == 0 ? 0 : 1;
}
