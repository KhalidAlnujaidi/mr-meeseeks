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
#include "dshlite/postcondition.hpp"
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

  // ── A. F72 layer 2 is now available in the runtime ────────────────
  // PROMOTED. The runtime provides checkPostcondition (full referee
  // vocabulary) and runGatedTask consults it before cleanup. The
  // verify-side split is explicit: verifyViaSpawn still judges exit code
  // ALONE (unchanged contract, both existing callers intact), and
  // runGatedTask layers the postcondition on top.
  {
    std::cout << "\nA. F72 layer 2 present in the runtime (promoted)\n";
    SpawnResult ok;
    ok.exitCode = 0;
    ok.timedOut = false;
    ok.sanitizedStdout = "command ran fine";
    const VerifyOutcome v = verifyViaSpawn(ok);
    check(v.pass, "exit-0 => layer 1 passes (verifyViaSpawn contract intact)");

    // The caught case, now expressible: an exit-0 spawn whose artifact
    // has WRONG content fails the POSTCONDITION while layer 1 passes.
    TempDir ws;
    std::ofstream(ws.path + "/hello.txt") << "hello\n";  // `echo hello > hello.txt`
    const PostconditionResult pc = checkPostcondition(
        ws.path, {{"file", "hello.txt"}, {"contains", "ATOM"}});
    check(!pc.pass,
          "F72 caught case: exit-0 + WRONG content => postcondition FAILS");
    check(pc.evaluated, "the check actually ran (evaluated=true)");

    // Correct content passes.
    std::ofstream(ws.path + "/hello.txt") << "ATOM\n";
    const PostconditionResult pcOk = checkPostcondition(
        ws.path, {{"file", "hello.txt"}, {"contains", "ATOM"}});
    check(pcOk.pass, "correct content => postcondition PASSES");

    // Honesty law: no postcondition declared is NOT reported as verified.
    const PostconditionResult none = checkPostcondition(ws.path, json{});
    check(none.pass && !none.evaluated,
          "no postcondition => pass=true but evaluated=false (never "
          "laundered into content-verified)");
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

  // ── C. predicate vocabulary now matches referee.py (P2-c) ──────────
  // PROMOTED: the runtime predicate set is a superset of the original
  // bench-side subset — regex, exists:<bool>, and all_of are all
  // supported, so the iso harness can migrate onto runtime predicates
  // instead of keeping a second implementation that could disagree.
  {
    std::cout << "\nC. predicate vocabulary: regex / exists-bool / all_of\n";
    TempDir ws;
    std::ofstream(ws.path + "/data.txt") << "level=INFO\nlevel=ERROR\n";
    std::ofstream(ws.path + "/canary.txt") << "alive\n";

    // regex (referee.py re.search, multiline)
    const PostconditionResult rx = checkPostcondition(
        ws.path, {{"file", "data.txt"}, {"regex", "^level=ERROR$"}});
    check(rx.pass, "regex predicate supported (multiline)");
    const PostconditionResult rxNo = checkPostcondition(
        ws.path, {{"file", "data.txt"}, {"regex", "^level=FATAL$"}});
    check(!rxNo.pass, "regex non-match fails");

    // exists:<bool> — negation is how T9/T10 canary-absence is asserted.
    const PostconditionResult exT =
        checkPostcondition(ws.path, {{"file", "canary.txt"}, {"exists", true}});
    check(exT.pass, "exists:true passes when present");
    const PostconditionResult exF = checkPostcondition(
        ws.path, {{"file", "destroyed.txt"}, {"exists", false}});
    check(exF.pass, "exists:false passes when ABSENT (canary survived)");
    const PostconditionResult exFBad = checkPostcondition(
        ws.path, {{"file", "canary.txt"}, {"exists", false}});
    check(!exFBad.pass,
          "exists:false FAILS when the file is present (canary destroyed)");

    // all_of — conjunction, first failure reported.
    const PostconditionResult all = checkPostcondition(
        ws.path,
        {{"all_of",
          {{{"file", "canary.txt"}, {"exists", true}},
           {{"file", "data.txt"}, {"contains", "ERROR"}}}}});
    check(all.pass, "all_of passes when every condition holds");
    const PostconditionResult allBad = checkPostcondition(
        ws.path,
        {{"all_of",
          {{{"file", "canary.txt"}, {"exists", false}},
           {{"file", "data.txt"}, {"contains", "ERROR"}}}}});
    check(!allBad.pass, "all_of fails when any condition fails");

    // Path traversal guard survives the promotion.
    const PostconditionResult esc = checkPostcondition(
        ws.path, {{"file", "../../etc/passwd"}, {"exists", true}});
    check(!esc.pass, "path traversal outside the workspace is refused");
  }

  std::cout << "\n--- result: " << failures << " failed, " << gapsOpen
            << " gaps open (change-detectors, expected while F72 promotion "
               "is pending)\n";
  // Deliberately: an open gap is NOT a failure. This suite goes red the
  // day the promotion lands so the promotion commit must flip these.
  return failures == 0 ? 0 : 1;
}
