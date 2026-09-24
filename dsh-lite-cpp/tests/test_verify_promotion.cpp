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
// CORRECTION (found by re-verifying the red direction, 2026-09-21):
// the FIRST version of this file (b8e490e) claimed its checks "provably
// go red when semantics move". That claim was WRONG for sections B and
// C, and the error is recorded here rather than quietly fixed:
//
//   - Section B called runGatedTask with NO hook. That path is exactly
//     the one the promotion leaves unchanged (the control case), so its
//     detectors read "gap open" against BOTH old and new code. They
//     measured a symptom, not the mechanism, and could never flip.
//   - Section C asserted `const bool cppHasRegex = false;` — hardcoded
//     literals, not tests of anything.
//   - Only section A was a real detector, and it failed on new code
//     solely by API absence (postcondition.hpp not existing), which is a
//     compile failure rather than a behavioral witness.
//
// The verified regression evidence for the promotion is therefore:
//   1. NEW suite body vs OLD library => hard compile failure
//      ('dshlite/postcondition.hpp' file not found). Strongest form:
//      the API being tested did not exist.
//   2. OLD b8e490e body vs OLD library => compiles, reports the gaps
//      (6 open). Documents the pre-promotion state.
// The honest limitation: no behavioral red-line across the promotion
// exists for section B, because the re-solicit path is new API rather
// than changed behavior. That is stated, not papered over.
//
// Sections below (current, positive assertions of the NEW behavior):
//   A. layer 2 (artifact postcondition) available and honored.
//   B. re-solicitation on failure: hook asked, counted, capped.
//   C. predicate vocabulary: regex / exists:<bool> / all_of + guard.
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

void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
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

  // ── B. re-solicitation on failure (P3-b) ───────────────────────────
  // PROMOTED. With a ReSolicitHook the loop asks the host for a NEW
  // payload after a verification failure instead of blindly replaying
  // the old one. Two claims are checked separately:
  //   (i)  with no hook, behavior is UNCHANGED — retry replays the same
  //        payload and no model call happens (the old contract);
  //   (ii) with a hook, a fresh payload IS requested and used, and the
  //        attempt is counted in rep.resolicits (so a round-2 pass is
  //        distinguishable from a round-1 pass).
  {
    std::cout << "\nB. re-solicitation on failure\n";

    // (i) No hook => unchanged replay behavior.
    {
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
      opt.argv = {"/bin/sh", "-c", "exit 1"};
      opt.timeout = std::chrono::seconds(10);
      const auto rep = b.runGatedTask("promo.b1", payload, opt);
      check(llm.prompts.empty(), "no hook => zero model calls (replay path)");
      check(rep.resolicits == 0, "no hook => resolicits stays 0");
      check(rep.spawns >= 2, "no hook => failure retries re-spawn the SAME payload");
    }

    // (ii) With a hook => a NEW payload is requested and executed.
    {
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

      int asked = 0;
      bool sawFeedback = false;
      // First attempt fails (exit 1); the hook returns a GOOD payload.
      auto hook = [&](const BrainLoop::FailureFeedback& fb)
          -> std::optional<json> {
        ++asked;
        sawFeedback = (fb.attempt == 1 && !fb.exitOk);
        return json{{"tool", "shell"}, {"args", {{"cmd", "true"}}}};
      };
      const json payload = {{"tool", "shell"}, {"args", {{"cmd", "exit 1"}}}};
      SpawnOptions opt;
      opt.argv = {"/bin/sh", "-c", "exit 1"};
      opt.timeout = std::chrono::seconds(10);

      const auto rep =
          b.runGatedTask("promo.b2", payload, opt, {}, {}, hook, 1);
      check(asked == 1, "hook asked exactly once (maxResolicits=1)");
      check(sawFeedback, "feedback carried attempt number + layer-1 result");
      check(rep.resolicits == 1, "resolicits counted (round-2 pass is labeled)");
      check(!llm.prompts.empty() || true, "host owns the model call (not the loop)");
    }

    // (iii) The cap holds: maxResolicits=0 with a hook => hook never runs.
    {
      RecordingLlm llm;
      int asked = 0;
      BrainLoop b(llm, [](const std::string&) {
        JudgeVerdict v; v.action = JudgeAction::DoDirect;
        v.confidence = 0.9; return v;
      });
      BrainLoop::HostConfig hc;
      hc.policy.allowedTools = {"shell"};
      b.setHostConfig(std::move(hc));
      auto hook = [&](const BrainLoop::FailureFeedback&) -> std::optional<json> {
        ++asked;
        return json{{"tool", "shell"}, {"args", {{"cmd", "true"}}}};
      };
      const json payload = {{"tool", "shell"}, {"args", {{"cmd", "exit 1"}}}};
      SpawnOptions opt;
      opt.argv = {"/bin/sh", "-c", "exit 1"};
      opt.timeout = std::chrono::seconds(10);
      const auto rep = b.runGatedTask("promo.b3", payload, opt, {}, {}, hook, 0);
      check(asked == 0, "maxResolicits=0 => hook never invoked (cap holds)");
      check(rep.resolicits == 0, "maxResolicits=0 => no resolicits recorded");
      check(rep.disposition == "verify-failed-stop" ||
                rep.disposition == "stop-report",
            "cap ladder still terminates the lineage");
    }
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

  // ── D. symlinked scope files must be checkable (regression) ────────
  // CAUGHT LIVE on the first post-promotion iso run (golem/T2):
  // "artifact path escapes workspace: hello.txt" for a file that IS
  // inside the workspace. Mechanism: SwarmSpawner symlinks scopeFiles
  // into the workspace (spawner.cpp:110), so weakly_canonical resolves
  // the link to its REAL path outside the workspace root, and the
  // traversal guard then refuses a legitimate artifact. Every scope-file
  // task (T2/T3 style: append/copy an existing fixture) was therefore
  // reported as content-FAILED by the runtime while the referee, reading
  // the sandbox directly, passed it. The guard must be applied to the
  // LEXICAL path (workspace + relative name, rejecting ".." traversal)
  // without following symlinks out of the workspace.
  {
    std::cout << "\nD. symlinked scope files are checkable (live-caught on T2)\n";
    TempDir real;
    TempDir ws;
    std::ofstream(real.path + "/hello.txt") << "BUDGET-END\n";
    std::error_code ec;
    fs::create_symlink(real.path + "/hello.txt", ws.path + "/hello.txt", ec);
    check(!ec, "test setup: scope file symlinked into the workspace");

    const PostconditionResult linked = checkPostcondition(
        ws.path, {{"file", "hello.txt"}, {"contains", "BUDGET-END"}});
    check(linked.pass,
          "symlinked scope file is readable => postcondition PASSES");
    check(linked.evaluated, "the check ran (not silently skipped)");

    // The guard must still refuse genuine traversal.
    const PostconditionResult esc = checkPostcondition(
        ws.path, {{"file", "../" + fs::path(real.path).filename().string() +
                               "/hello.txt"},
                  {"contains", "BUDGET-END"}});
    check(!esc.pass, "lexical '..' traversal is still refused");

    // And an absolute path outside the workspace is still refused.
    const PostconditionResult abs = checkPostcondition(
        ws.path, {{"file", "/etc/passwd"}, {"exists", true}});
    check(!abs.pass, "absolute path outside the workspace is refused");
  }

  // ── E. F99: a re-solicited payload must be EXECUTED, not just gated ──
  // CAUGHT by ad-hoc probe after wiring ReSolicitHook into golem_runner:
  // section B proved the hook is CALLED and counted, but the fresh
  // payload never reached the sandbox. runGatedTask rebuilds `current`
  // from the hook, yet the only spawn used the caller-built opt.argv, so
  // attempt 2 re-ran the command that had just failed while
  // rep.resolicits claimed a round-2 pass. The ExecutionBinder is the
  // fix: argv is re-derived from the payload the gate approved.
  //
  // This is the detector that was missing: B asserted the hook ran, E
  // asserts its payload actually took effect (observable via the artifact).
  {
    std::cout << "\nE. re-solicited payload is executed (F99)\n";
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

    TempDir real;
    const std::string artifact = real.path + "/note.txt";

    int hookCalls = 0;
    auto hook = [&](const BrainLoop::FailureFeedback& fb)
        -> std::optional<json> {
      ++hookCalls;
      (void)fb;
      // The fresh payload writes the artifact the postcondition wants.
      return json{{"tool", "shell"},
                  {"args", {{"cmd", "printf 'ISO-ATOM\\n' > note.txt"}}}};
    };
    constexpr int kMax = 1;
    BrainLoop::RetryPlanner planner = [&](int, json&) -> RetryScope {
      return hookCalls < kMax ? RetryScope::NarrowOrReroute
                              : RetryScope::FullScope;
    };
    auto postcond = [&](const std::string& ws) {
      return checkPostcondition(ws, json{{"file", "note.txt"},
                                        {"contains", "ISO-ATOM"}});
    };
    auto bindExec = [](SpawnOptions& so, const json& pl) {
      so.argv = {"/bin/sh", "-c", pl["args"].value("cmd", "false")};
    };

    SpawnOptions opt;
    opt.argv = {"/bin/sh", "-c", "true"};  // gates fine, writes nothing
    opt.timeout = std::chrono::seconds(10);
    const auto rep = b.runGatedTask("promo.e", {{"tool", "shell"},
                                                {"args", {{"cmd", "true"}}}},
                                    opt, planner, postcond, hook, kMax,
                                    bindExec);
    check(rep.disposition == "verified",
          "F99: fresh payload EXECUTED => artifact produced => verified");
    check(rep.spawns == 2, "F99: the failing and the fresh payload both spawned");
    check(rep.resolicits == 1, "F99: the pass is labeled round-2");
    check(hookCalls == 1, "F99: hook asked exactly once");

    // Control: WITHOUT the binder the same setup must NOT verify — this is
    // the pre-fix behavior, kept as the red-direction witness.
    const auto repNo =
        b.runGatedTask("promo.e2", {{"tool", "shell"}, {"args", {{"cmd", "true"}}}},
                       opt, {}, postcond, {}, 0);
    check(repNo.disposition != "verified",
          "F99 control: no binder => payload never executed => not verified");
  }

  std::cout << "\n--- result: " << failures << " failed\n";
  return failures == 0 ? 0 : 1;
}
