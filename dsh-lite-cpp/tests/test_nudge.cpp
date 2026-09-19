// test_nudge.cpp — Gap 2 acceptance (docs/colibri-roadmap.md):
//   G2.2 nudge-depth + retry-cap state machine (caps ported verbatim
//        from harness/loop.ts + skills/forward-nudge/GUARDRAILS.md),
//   G2.3 repeat-loop stall variant,
//   G2.4 pre-execution payload gate (F7),
//   G2.5 history compaction + E.2.3 result vectors (F1),
//   G2.6 local verification gate (F9).
// Pure state machine + gate + compaction: NO sockets in this suite.
//
// Regression honesty (standing rule 3): every module asserted here did
// not exist pre-Gap-2 — this suite does not even COMPILE against the
// pre-gate tree (headers absent), which is the strongest possible fail;
// per-check notes below name the pre-gate behavior each check replaces.

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/compaction.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/payload_gate.hpp"
#include "dshlite/sanitizer.hpp"
#include "dshlite/spawner.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

bool throwsWith(const std::function<void()>& fn, const std::string& needle) {
  try {
    fn();
  } catch (const std::exception& e) {
    return std::string(e.what()).find(needle) != std::string::npos;
  } catch (...) {
    return false;
  }
  return false;
}
}  // namespace

int main() {
  using namespace dshlite;

  // ── G2.2 nudge depth cap (MAX_NUDGE_DEPTH = 3, I7) ─────────────────
  {
    NudgeState s;
    s.taskId = "R2.1";
    check(nudgeAllowed(s), "nudge 1 allowed at depth 0");
    recordNudge(s, "stall-no-bytes");
    recordNudge(s, "stall-no-bytes");
    recordNudge(s, "repeat-loop");
    check(s.nudgeDepth == 3, "depth counts to 3");
    check(s.lastReason == "repeat-loop", "lastReason recorded (ledger nudge line)");
    check(!nudgeAllowed(s),
          "G2.2: 4th nudge REFUSED by construction (depth 3/3, maxNudgeDepth=3)");
    // Pre-gate behavior replaced: NO cap existed in C++ at all — a host
    // loop could re-prompt forever. Fail-loud throw names cap+recovery:
    check(throwsWith([&] { recordNudge(s, "one too many"); }, "maxNudgeDepth=3"),
          "4th recordNudge throws naming the cap (fail loud)");
    check(throwsWith([&] { recordNudge(s, "x"); }, "recordUserTurn"),
          "throw message names the recovery (explicit user turn)");
    check(s.nudgeDepth == 3, "refused nudge did NOT increment depth");

    // User turn resets depth (I7) but NOT retry rounds (F27).
    s.rounds = 1;
    recordUserTurn(s);
    check(s.nudgeDepth == 0 && nudgeAllowed(s),
          "I7: explicit user turn resets nudgeDepth to 0");
    check(s.rounds == 1,
          "F27: user turn does NOT launder per-task retry rounds (I6)");
    recordNudge(s, "after user turn");
    check(s.nudgeDepth == 1, "nudges allowed again after reset");

    // A stopped lineage stays stopped even across a user turn.
    NudgeState dead;
    dead.stopped = true;
    recordUserTurn(dead);
    check(!nudgeAllowed(dead), "stopped lineage: nudge refused until re-tasked");
  }

  // ── G2.2 retry caps (MAX_ROUNDS_PER_TASK = 2, I6) ──────────────────
  {
    NudgeState s;
    s.taskId = "R2.2";
    check(recordRetry(s, RetryScope::FullScope) == RetryVerdict::Allow &&
              s.rounds == 1,
          "retry 1 of 2 allowed (I6)");
    check(recordRetry(s, RetryScope::FullScope) == RetryVerdict::Allow &&
              s.rounds == 2,
          "retry 2 of 2 allowed (I6)");
    // 3rd FULL-scope retry: rejected by construction — doctrine rule 5,
    // never silently redo full-scope labor. Pre-gate: no C++ retry
    // accounting existed; the host could loop verifications forever.
    check(recordRetry(s, RetryScope::FullScope) ==
              RetryVerdict::MustNarrowOrReroute,
          "G2.2: 3rd full-scope retry => MustNarrowOrReroute (rejected)");
    check(s.rounds == 2, "MustNarrowOrReroute did NOT consume a round");
    // 3rd attempt narrower or re-routed to a different family: allowed.
    check(recordRetry(s, RetryScope::NarrowOrReroute) == RetryVerdict::Allow &&
              s.rounds == 3,
          "3rd attempt allowed ONLY narrower/re-routed (different family)");
    // Anything past the narrowed 3rd: stop+report, latched.
    check(recordRetry(s, RetryScope::FullScope) == RetryVerdict::StopAndReport &&
              s.stopped,
          "4th retry => StopAndReport, lineage latched stopped");
    check(recordRetry(s, RetryScope::NarrowOrReroute) ==
              RetryVerdict::StopAndReport,
          "stopped lineage refuses even narrowed retries (stop+report)");
    check(!nudgeAllowed(s), "stopped lineage also refuses nudges");
  }

  // ── G2.3 repeat-loop stall variant ─────────────────────────────────
  {
    check(isRepeatLoop("same line\nsame line\nsame line\nsame line\n"
                       "same line\nsame line\nsame line\nsame line\n"),
          "G2.3: same line x8 => repeat-loop (stall variant, not success)");
    check(!isRepeatLoop("same line\nsame line\nsame line\nsame line\n"
                        "same line\nsame line\nsame line\n"),
          "7 consecutive repeats below k=8 => not degenerate");
    check(!isRepeatLoop("a\nb\na\nb\na\nb\na\nb\na\nb\n"),
          "alternating lines are not a consecutive run");
    check(isRepeatLoop("tok\n\n\n\ntok\ntok\ntok\ntok\ntok\ntok\ntok", 7),
          "empty lines are formatting, not part of the run");
    check(!isRepeatLoop(""), "empty output is not a repeat loop");
    check(std::string(kRepeatLoopReason) == "repeat-loop",
          "ledger reason string is exactly \"repeat-loop\" (G2.3)");
  }

  // ── G2.6 local verification gate (F9) ──────────────────────────────
  {
    SpawnResult ok;
    ok.exitCode = 0;
    ok.timedOut = false;
    ok.sanitizedStdout = "all checks passed";
    check(verifyViaSpawn(ok).pass, "G2.6: exit 0 => local verify PASS (no judge)");
    SpawnResult bad;
    bad.exitCode = 2;
    bad.sanitizedStderr = "assert failed \x1b[31mRED\x1b[0m";
    auto vb = verifyViaSpawn(bad);
    check(!vb.pass && vb.exitCode == 2, "G2.6: non-zero exit => FAIL, sycophancy-immune");
    check(vb.detail.find("\x1b") == std::string::npos,
          "verify detail sanitized via Module 1 (ANSI stripped)");
    SpawnResult to;
    to.exitCode = 124;
    to.timedOut = true;
    check(!verifyViaSpawn(to).pass, "watchdog timeout never passes verification");

    // Startup law: remote-judge keys must be absent in this lane.
    ::unsetenv("TYPESAFE_API_KEY");
    ::unsetenv("OPENROUTER_API_KEY");
    bool noThrow = true;
    try {
      assertNoRemoteJudgeEnv();
    } catch (...) {
      noThrow = false;
    }
    check(noThrow, "G2.6/F9: clean env passes assertNoRemoteJudgeEnv");
    ::setenv("TYPESAFE_API_KEY", "secret", 1);
    check(throwsWith([] { assertNoRemoteJudgeEnv(); }, "TYPESAFE_API_KEY"),
          "G2.6/F9: TYPESAFE_API_KEY present => loud startup refusal");
    ::unsetenv("TYPESAFE_API_KEY");
    ::setenv("OPENROUTER_API_KEY", "secret", 1);
    check(throwsWith([] { assertNoRemoteJudgeEnv(); }, "OPENROUTER_API_KEY"),
          "G2.6/F9: OPENROUTER_API_KEY present => loud startup refusal");
    ::unsetenv("OPENROUTER_API_KEY");
  }

  // ── G2.4 pre-execution payload gate (F7) ───────────────────────────
  {
    PolicyConfig policy;
    policy.allowedTools = {"read_file", "write_file", "run_tests", "git"};

    // (a) schema: clean payload passes.
    auto v = checkPayload(nlohmann::json::parse(
        R"({"tool":"read_file","args":{"path":"src/main.cpp"}})"), policy);
    check(v.allowed && v.code == "OK" && autoExecutable(v),
          "G2.4: schema-valid allowlisted payload => OK, auto-executable");

    // schema failures: not an object / missing tool / missing args /
    // args not an object. Pre-gate: dsh-lite had NO payload validation
    // at all — plain role/content text went straight to the spawner
    // (that absence IS F7).
    check(!checkPayload(nlohmann::json::parse("\"rm -rf /\""), policy).allowed &&
              checkPayload(nlohmann::json::parse("\"rm -rf /\""), policy).code ==
                  "PAYLOAD_SCHEMA_INVALID",
          "non-object payload refused PAYLOAD_SCHEMA_INVALID");
    check(checkPayload(nlohmann::json::parse(R"({"args":{}})"), policy).code ==
              "PAYLOAD_SCHEMA_INVALID",
          "missing \"tool\" refused");
    check(checkPayload(nlohmann::json::parse(R"({"tool":"git"})"), policy).code ==
              "PAYLOAD_SCHEMA_INVALID",
          "missing \"args\" refused");
    check(checkPayload(nlohmann::json::parse(R"({"tool":"git","args":"x"})"),
                       policy)
              .code == "PAYLOAD_SCHEMA_INVALID",
          "non-object \"args\" refused");

    // (c) unknown tool => TOOL_NOT_ALLOWED — even when destructive
    // (F21 ordering: a destructive verb never launders an unknown tool
    // into propose-only).
    auto unk = checkPayload(
        nlohmann::json::parse(R"({"tool":"delete-everything","args":{}})"),
        policy);
    check(!unk.allowed && unk.code == "TOOL_NOT_ALLOWED",
          "F21: unknown destructive tool => REFUSED, not propose-only");

    // (b) destructive scan: tool args + tool name, recursive.
    auto push = checkPayload(
        nlohmann::json::parse(R"({"tool":"git","args":{"cmd":"push origin main"}})"),
        policy);
    check(push.allowed && push.code == "DESTRUCTIVE_PROPOSE_ONLY" &&
              !autoExecutable(push),
          "A.3: \"push\" in args => allowed to PROPOSE only, never auto-execute");
    auto nested = checkPayload(
        nlohmann::json::parse(
            R"({"tool":"run_tests","args":{"env":{"SECRET":"x"},"cmd":["make","rm -rf build"]}})"),
        policy);
    check(nested.code == "DESTRUCTIVE_PROPOSE_ONLY",
          "destructive verb found recursively in nested string args");
    auto info = checkPayload(
        nlohmann::json::parse(
            R"({"tool":"read_file","args":{"path":"docs/information.md","note":"reformat later"}})"),
        policy);
    check(info.code == "OK",
          "F21: word-boundary scan — \"information\"/\"reformat\" do NOT trip \"format\"");
    auto fmt = checkPayload(
        nlohmann::json::parse(R"({"tool":"run_tests","args":{"cmd":"format /dev/sda"}})"),
        policy);
    check(fmt.code == "DESTRUCTIVE_PROPOSE_ONLY",
          "standalone \"format\" DOES trip the destructive scan");

    // enforceGateBeforeSpawn: the call-site contract helper.
    bool threw = false;
    try {
      enforceGateBeforeSpawn(unk);
    } catch (const std::runtime_error& e) {
      threw = std::string(e.what()).find("TOOL_NOT_ALLOWED") != std::string::npos;
    }
    check(threw, "enforceGateBeforeSpawn throws loudly on refused payload");
    check(enforceGateBeforeSpawn(push) == false,
          "enforceGateBeforeSpawn: destructive => returns false (propose-only, do NOT spawn)");
    check(enforceGateBeforeSpawn(v) == true,
          "enforceGateBeforeSpawn: clean payload => true (may spawn)");
  }

  // ── G2.5 history compaction (F1: vector, no deque) ─────────────────
  {
    CompactionConfig cfg;  // defaults: threshold 8192, head 4096, tail 1024
    // Fixture law: middle turns must total ABOVE thresholdChars, else
    // compaction is a legitimate no-op and this test proves nothing.
    // (Original 1500-char fixture summed to 7,643 < 8,192 — caught in
    // the child's post-mortem; 2500 x 5 middle turns => ~12.6k chars.)
    std::vector<Message> h = {
        {"system", "You are the Budget-AGI brain."},
        {"user", "task R1"},
        {"assistant", std::string(2500, 'a')},
        {"user", std::string(2500, 'b')},
        {"assistant", std::string(2500, 'c') + "\x1b[31mANSI\x1b[0m"},
        {"user", std::string(2500, 'd')},
        {"assistant", std::string(2500, 'e')},
        {"user", "final question"},
    };
    const long before = serializedChars(h);
    check(before > cfg.thresholdChars, "test history exceeds the 8192 budget");

    // Short history: untouched no-op (pre-gate: BrainLoop had NO
    // compaction at all — history_ grew without bound, that is F1).
    std::vector<Message> small = {{"system", "s"}, {"user", "hi"}};
    check(!compactHistory(small, cfg) && small.size() == 2,
          "under-budget history is a no-op");

    check(compactHistory(h, cfg), "over-budget history compacted (changed=true)");
    check(serializedChars(h) <= cfg.thresholdChars,
          "compacted history fits the thresholdChars budget");
    check(h.front().role == "system" &&
              h.front().content == "You are the Budget-AGI brain.",
          "G2.5 law: system message (index 0) NEVER touched");
    check(h.back().role == "user" && h.back().content == "final question",
          "G2.5 law: final user message NEVER touched");
    bool markerFound = false, ansiFound = false;
    for (const auto& m : h) {
      if (m.content.rfind(kCompactionMarker, 0) == 0) markerFound = true;
      if (m.content.find("\x1b") != std::string::npos) ansiFound = true;
    }
    check(markerFound, "pruned middle turns collapse into one [HOST COMPACTION] marker");
    check(!ansiFound, "re-injected/kept-through-compaction text carries no ANSI (Module 1)");

    // Head/tail truncation stage with lowered caps (F23: at default
    // 4096+1024 the sanitizer's own 4096 cap fires first — lowered
    // caps prove the stage itself works).
    std::vector<Message> h2 = {
        {"system", "sys"},
        {"user", "go"},
        {"assistant", std::string(300, 'x')},
        {"user", "last"},
    };
    CompactionConfig tight;
    tight.thresholdChars = 200;  // stage-1 truncation alone must satisfy it
    tight.headChars = 20;
    tight.tailChars = 10;
    check(compactHistory(h2, tight), "tight caps trigger per-message truncation");
    check(h2[2].content.size() < 300 &&
              h2[2].content.find(TRUNCATION_MARKER) != std::string::npos,
          "head(headChars)+marker+tail(tailChars) semantics applied per-message");
    check(h2[0].content == "sys" && h2[3].content == "last",
          "protected messages untouched by truncation stage");

    // Honest non-achievement: protected-only overflow is reported, not lied about.
    std::vector<Message> h3 = {
        {"system", std::string(9000, 's')},
        {"user", std::string(9000, 'u')},
    };
    compactHistory(h3, cfg);
    check(h3.size() == 2 && serializedChars(h3) > cfg.thresholdChars,
          "protected messages are never pruned even when over budget (honest)");
  }

  // ── G2.5 E.2.3 result vector ───────────────────────────────────────
  {
    const auto good = nlohmann::json::parse(R"({
      "taskId": "task-8042",
      "status": "success",
      "summary": "Implemented JWT auth middleware and added unit tests.",
      "changedFiles": ["src/middleware/auth.ts", "tests/auth.test.ts"],
      "gitDiffHash": "a1b2c3d4",
      "exportedState": {"jwtHeaderKey": "X-Auth-Token", "retries": 2, "ok": true}
    })");
    auto r = toResultVector(good, /*hasPatchFile=*/true);
    check(r.ok && r.vector.status == "SUCCESS",
          "E.2.3: lowercase \"success\" normalizes to canonical SUCCESS");
    check(r.vector.gitDiffHash == "a1b2c3d4" && r.vector.changedFiles.size() == 2,
          "E.2.3: gitDiffHash binds when the patch exists; changedFiles kept");

    // Status enum: anything else rejected (pre-gate: no C++ validator).
    auto badStatus = good;
    badStatus["status"] = "MAYBE";
    check(!toResultVector(badStatus, true).ok &&
              toResultVector(badStatus, true).code == "VECTOR_FIELD_INVALID",
          "E.2.3: unknown status rejected (canonical enum only)");

    // Summary line bound 1-5.
    auto sixLines = good;
    sixLines["summary"] = "l1\nl2\nl3\nl4\nl5\nl6";
    check(!toResultVector(sixLines, true).ok,
          "E.2.3: 6-line summary rejected (1-5 lines)");
    auto fiveLines = good;
    fiveLines["summary"] = "l1\nl2\nl3\nl4\nl5";
    check(toResultVector(fiveLines, true).ok, "5-line summary accepted");

    // gitDiffHash conditional binding, both directions.
    check(!toResultVector(good, /*hasPatchFile=*/false).ok,
          "E.2.3: hash supplied with NO patch file => rejected");
    auto noHash = good;
    noHash["gitDiffHash"] = "";
    check(!toResultVector(noHash, true).ok,
          "E.2.3: empty hash WITH patch file => rejected (bindPatchHash)");
    check(toResultVector(noHash, false).ok,
          "E.2.3: FAILED/DISCARDED no-patch => \"\" accepted");

    // exportedState: oversize => BUS_VALUE_TOO_LARGE, REFUSE not truncate.
    auto fat = good;
    fat["exportedState"] = {{"blob", std::string(5000, 'z')}};
    auto rf = toResultVector(fat, true);
    check(!rf.ok && rf.code == "BUS_VALUE_TOO_LARGE",
          "E.2.3: exportedState > 4 KiB => BUS_VALUE_TOO_LARGE refuse");
    check(rf.message.find("NEVER truncates") != std::string::npos,
          "refusal message states the no-truncation law (fail loud)");
    // non-scalar values => same code (spec E.2.3 nit).
    auto nestedState = good;
    nestedState["exportedState"] = {{"nested", {{"a", 1}}}};
    check(toResultVector(nestedState, true).code == "BUS_VALUE_TOO_LARGE",
          "E.2.3: nested (non-scalar) exportedState => BUS_VALUE_TOO_LARGE");

    // changedFiles must be repo-relative strings.
    auto abs = good;
    abs["changedFiles"] = {"/etc/passwd"};
    check(!toResultVector(abs, true).ok, "E.2.3: absolute changedFiles path refused");

    // Wire JSON round-trip carries every schema key.
    const auto j = resultVectorJson(r.vector);
    check(j.contains("taskId") && j.contains("status") && j.contains("summary") &&
              j.contains("changedFiles") && j.contains("gitDiffHash") &&
              j.contains("exportedState"),
          "E.2.3: resultVectorJson emits the full schema");
  }

  std::cout << (failures == 0 ? "NUDGE PASS\n" : "NUDGE FAIL\n");
  return failures == 0 ? 0 : 1;
}
