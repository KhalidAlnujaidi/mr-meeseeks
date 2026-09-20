// test_g4_stress.cpp — Gap 4 deep forced-failure stress (roadmap G4,
// F40-F42). OFFLINE by design (F40): deterministic stub engines + real
// /bin/sh spawns exercise the host-enforced stack; live-engine telemetry
// is g4-run's job. All ledger lines append to /tmp/g4-ledger.jsonl with
// task ids tagged "g4s.*" so stub stress data never mixes unlabeled with
// live-run data (F42).
//
// Stresses, with the BINDING CAP labeled per scenario (F41):
//   a) I7 maxNudgeDepth=3 — state machine directly (4th nudge throws by
//      construction) + through runGatedTask's retry ladder (nudgeDepth
//      never exceeds 3; I6 rounds bind first — labeled).
//   b) worker->worker fallback — dead engine first, healthy second;
//      attempt log + brain-pool invariance.
//   c) history compaction under large tool outputs — multi-turn stub
//      replies of ~3 KB each blow the 8192 budget; compaction must keep
//      history in budget with system/final/last-user protected.
//   d) injected payload/schema violations — unknown tool, non-object
//      args, destructive verb: zero spawns on every refusal, DENY/
//      propose-only ledger lines.
//   e) F39 heat wiring end-to-end — synthetic .coli_usage attached to
//      brain-turn report lines; cache block present in the real ledger.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/compaction.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/router.hpp"
#include "dshlite/usage_probe.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

const std::string kLedgerPath =
    (std::filesystem::temp_directory_path() / "golem-g4s-ledger.jsonl").string();
const char* kLedger = kLedgerPath.c_str();

// Stub SSE engine: streams `bodyChars` of content as deltas (big-reply
// knob for the compaction stress), then usage, then [DONE].
struct StressEngine {
  httplib::Server srv;
  std::thread th;
  int port = -1;
  std::string servedId;
  int bodyChars = 60;
  std::atomic<long> hits{0};

  void start(const std::string& id, int portHint, int chars = 60) {
    servedId = id;
    bodyChars = chars;
    srv.Post("/v1/chat/completions",
             [this](const httplib::Request& req, httplib::Response& res) {
               hits.fetch_add(1);
               if (req.body.find("\"" + servedId + "\"") == std::string::npos) {
                 res.status = 404;
                 res.set_content("{}", "application/json");
                 return;
               }
               const std::string body(bodyChars, 'z');
               const long completion = std::max(1L, bodyChars / 4L);
               res.set_chunked_content_provider(
                   "text/event-stream",
                   [this, body, completion](size_t, httplib::DataSink& sink) {
                     auto send = [&sink](const std::string& frame) {
                       std::string chunk = "data: " + frame + "\n\n";
                       sink.write(chunk.data(), chunk.size());
                     };
                     // two content deltas + usage + DONE (keepalive-free)
                     send(R"({"choices":[{"delta":{"content":")" + body + R"("}}]})");
                     send(R"({"choices":[{"delta":{"content":""}}],)"
                          R"("finish_reason":"stop"})");
                     send(R"({"choices":[],"usage":{"prompt_tokens":30,)"
                          R"("completion_tokens":)" +
                          std::to_string(completion) + R"(,"total_tokens":)" +
                          std::to_string(30 + completion) + "}}");
                     send("[DONE]");
                     sink.done();
                     return true;
                   });
             });
    for (int p = portHint; p < portHint + 40; ++p) {
      httplib::Server probe;
      if (!probe.bind_to_port("127.0.0.1", p)) continue;
      port = p;
      break;
    }
    if (port == -1) return;
    th = std::thread([this] { srv.listen("127.0.0.1", port); });
    for (int i = 0; i < 100 && !srv.is_running(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  void stop() {
    if (port == -1) return;
    srv.stop();
    if (th.joinable()) th.join();
  }
  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  }
};

// Brain turns through the router pool, ledger + heat attached (F39).
struct StressPoster : dshlite::ILlmPoster {
  dshlite::ModelRouter& router;
  dshlite::LedgerWriter& ledger;
  std::string usagePath;
  std::string tag;
  long turnNo = 0;
  StressPoster(dshlite::ModelRouter& r, dshlite::LedgerWriter& l, std::string t)
      : router(r), ledger(l), tag(std::move(t)) {}
  dshlite::LlmResponse post(const std::vector<dshlite::Message>& m) override {
    auto rr = router.post(dshlite::Role::Brain, m);
    ++turnNo;
    auto ev = dshlite::LedgerWriter::fromRouted(
        "report", "brain", tag + ".brain-turn" + std::to_string(turnNo), 0, rr);
    if (!usagePath.empty())
      dshlite::attachCacheHeat(ev, dshlite::probeUsageFile(usagePath));
    ledger.append(ev);
    return rr.response;
  }
};

dshlite::SpawnOptions sh(const char* script) {
  dshlite::SpawnOptions o;
  o.argv = {"/bin/sh", "-c", script};
  o.timeout = std::chrono::seconds(15);
  return o;
}
}  // namespace

int main() {
  using namespace dshlite;

  StressEngine brainEng, healthy;
  brainEng.start("glm-5.2-colibri", 18800, /*chars=*/3000);  // big replies: (c)
  healthy.start("olmoe-colibri", 18860);
  check(brainEng.port != -1 && healthy.port != -1, "stress stub engines up");
  if (brainEng.port == -1 || healthy.port == -1) {
    std::cout << "G4STRESS FAIL\n";
    return 1;
  }

  // Synthetic heat file (e): fresh mtime => warm=true in the ledger.
  const std::string heatDir = (std::filesystem::temp_directory_path() /
                                 ("golem-g4s-" + std::to_string(::getpid()))).string();
  ::mkdir(heatDir.c_str(), 0755);
  const std::string heatPath = heatDir + "/coli_usage";
  { std::ofstream o(heatPath); o << "-1 16 64\n-2 1 7\n0 3 12\n"; }

  LedgerWriter ledger(kLedger);

  RouterConfig rc;
  EngineEntry bE;
  bE.endpoint = brainEng.url();
  bE.modelId = "glm-5.2-colibri";
  bE.stream = true;
  bE.timeout = std::chrono::seconds(30);
  bE.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
  rc.brain = {bE};
  // (b) worker pool: DEAD engine first, healthy second => forced fallback.
  EngineEntry dead;
  dead.endpoint = "http://127.0.0.1:1/v1/chat/completions";
  dead.modelId = "qwen3.8-flash-next-colibri";
  dead.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
  dead.timeout = std::chrono::seconds(2);
  EngineEntry hE;
  hE.endpoint = healthy.url();
  hE.modelId = "olmoe-colibri";
  hE.stream = true;
  hE.timeout = std::chrono::seconds(30);
  hE.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
  rc.worker = {dead, hE};
  rc.verifier = {hE};
  ModelRouter router(rc);

  StressPoster poster(router, ledger, "g4s");
  poster.usagePath = heatPath;
  BrainLoop brain(poster, [](const std::string&) -> JudgeVerdict {
    JudgeVerdict v;
    v.action = JudgeAction::DoDirect;
    v.route = "do_direct";
    v.confidence = 0.9;
    v.detail = "local heuristic judge (stress)";
    return v;
  });
  BrainLoop::HostConfig hc;
  hc.policy.allowedTools = {"shell"};
  hc.ledger = &ledger;
  brain.setHostConfig(std::move(hc));
  brain.setSystemPrompt("You are the Budget-AGI brain (stress run).");

  // ── (a) I7 nudge cap, state machine directly (F41: labeled) ────────
  {
    NudgeState st;
    st.taskId = "g4s.nudge-direct";
    recordNudge(st, "s1");
    recordNudge(st, "s2");
    recordNudge(st, "s3");
    bool fourthThrew = false;
    try {
      recordNudge(st, "s4");
    } catch (const std::logic_error&) {
      fourthThrew = true;
    }
    check(fourthThrew && st.nudgeDepth == kMaxNudgeDepth,
          "a/I7: 4th chained nudge throws by construction (depth pinned at 3)");
    recordUserTurn(st);
    check(st.nudgeDepth == 0, "a/I7: user turn resets nudgeDepth");
  }

  // ── (a') runGatedTask retry ladder: which cap binds (F41 label) ────
  {
    const nlohmann::json ok = {{"tool", "shell"}, {"args", {{"cmd", "false"}}}};
    // Default (FullScope) planner: I6 rounds bind — 3 spawns, then stop.
    auto full = brain.runGatedTask("g4s.ladder-full", ok, sh("exit 1"));
    check(full.disposition == "verify-failed-stop" && full.spawns == 3,
          "a'/I6 BINDS (full-scope): 3 spawns then verify-failed-stop");
    // Narrow planner: ladder extends to rounds=3 (one more than
    // full-scope) because each retry is narrower/re-routed; I6 still
    // binds at rounds>2. nudgeDepth reaches 3 but the 4th nudge is
    // unreachable through runGatedTask BY CONSTRUCTION (F41: labeled).
    auto narrow = brain.runGatedTask(
        "g4s.ladder-narrow", ok, sh("exit 1"),
        [](int, nlohmann::json&) { return RetryScope::NarrowOrReroute; });
    const NudgeState& ns = brain.nudgeState("g4s.ladder-narrow");
    check(narrow.disposition == "verify-failed-stop" && narrow.spawns == 4 &&
              ns.nudgeDepth <= kMaxNudgeDepth && ns.stopped,
          "a'/I6 BINDS (narrow): 4 spawns (rounds->3), nudgeDepth<=3, latched");
  }

  // ── (d) injected payload/schema violations: zero spawns each ───────
  {
    const nlohmann::json unknownTool = {{"tool", "curl"},
                                        {"args", {{"url", "http://x"}}}};
    const nlohmann::json badArgs = {{"tool", "shell"}, {"args", "not-an-object"}};
    const nlohmann::json destructive = {{"tool", "shell"},
                                        {"args", {{"cmd", "git push origin main"}}}};
    auto r1 = brain.runGatedTask("g4s.viol-unknown", unknownTool, sh("exit 0"));
    auto r2 = brain.runGatedTask("g4s.viol-schema", badArgs, sh("exit 0"));
    auto r3 = brain.runGatedTask("g4s.viol-destructive", destructive, sh("exit 0"));
    check(r1.disposition == "gate-refused" && r1.spawns == 0 &&
              r1.gate.code == "TOOL_NOT_ALLOWED",
          "d: unknown tool => refused, 0 spawns");
    check(r2.disposition == "gate-refused" && r2.spawns == 0 &&
              r2.gate.code == "PAYLOAD_SCHEMA_INVALID",
          "d: schema violation (args not object) => refused, 0 spawns");
    check(r3.disposition == "propose-only" && r3.spawns == 0 &&
              r3.gate.code == "DESTRUCTIVE_PROPOSE_ONLY",
          "d: destructive verb => propose-only, 0 spawns");
  }

  // ── (b) worker->worker fallback under a dead first engine ─────────
  {
    const long brainBefore = brainEng.hits.load();
    auto rr = router.post(Role::Worker, {{"user", "atom"}});
    check(rr.servedBy == healthy.url() && rr.attempts.size() == 2 &&
              rr.attempts[0].outcome == "transport" &&
              rr.attempts[1].outcome == "ok",
          "b: dead engine => worker->worker fallthrough (never brain)");
    check(brainEng.hits.load() == brainBefore,
          "b/F2: brain engine untouched by worker fallback");
  }

  // ── (c) compaction under large tool outputs (8 deep turns) ────────
  {
    for (int i = 0; i < 8; ++i)
      brain.turn("stress turn " + std::to_string(i) +
                 ": explain atomic decomposition briefly");
    const auto& h = brain.history();
    CompactionConfig cc;  // defaults: 8192 budget
    // F44 (design, not bug): turn() compacts BEFORE the post (F28 — the
    // model must never see over-budget context), then appends the reply
    // it just generated, which is itself protected (you never prune the
    // latest answer). So post-turn history can exceed the budget by one
    // protected reply. The real invariant is: the context the model SAW
    // (history minus that final reply) fits the budget.
    check(h.size() < 17, "c: compaction pruned middle turns (not 8x2+1=17 raw)");
    long finalReply = 0;
    if (!h.empty())
      finalReply = static_cast<long>(h.back().role.size() + h.back().content.size() + 4);
    check(h.back().role == "assistant", "c: final message is the just-generated reply");
    check(serializedChars(h) - finalReply <= cc.thresholdChars,
          "c/F28: pre-post context (history minus final protected reply) fits budget");
    check(serializedChars(h) < 8 * 3000,
          "c: 8 x ~3KB replies did NOT accumulate unbounded (compaction active)");
    bool marker = false;
    for (const auto& m : h)
      if (m.content.find(kCompactionMarker) != std::string::npos) marker = true;
    check(marker, "c: [HOST COMPACTION] marker present after deep turns");
    check(h.front().role == "system", "c: system message protected");
    check(h.back().role == "assistant", "c: latest reply intact");
    // Every brain turn emitted a ledger report line WITH cache block (e):
  }

  // ── (e) F39 heat wiring in the real ledger file ────────────────────
  {
    std::ifstream in(kLedger);
    std::string line;
    long brainLines = 0, cacheLines = 0, warmLines = 0, deny = 0, nudge = 0,
         verifyFail = 0, g4s = 0;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      const auto j = nlohmann::json::parse(line);  // must parse: F15
      const std::string tid = j.value("task_id", "");
      if (tid.rfind("g4s.", 0) == 0) ++g4s;
      if (j.value("type", "") == "report" && j.value("role", "") == "brain" &&
          tid.rfind("g4s.", 0) == 0) {
        // F43: scope to THIS run's tag — the shared ledger also holds
        // earlier live-run lines (g4.*, no cache block); counting those
        // would spuriously fail cacheLines==brainLines.
        ++brainLines;
        if (j.contains("cache")) {
          ++cacheLines;
          if (j["cache"].value("warm", false)) ++warmLines;
        }
      }
      if (j.value("type", "") == "DENY" && tid.rfind("g4s.", 0) == 0) ++deny;
      if (j.value("type", "") == "nudge" && tid.rfind("g4s.", 0) == 0) ++nudge;
      if (j.value("type", "") == "verify" && j.value("verdict", "") == "fail" &&
          tid.rfind("g4s.", 0) == 0)
        ++verifyFail;
    }
    check(g4s > 0, "F42: stress lines tagged g4s.* (separable from live runs)");
    check(brainLines >= 8 && cacheLines == brainLines && warmLines == brainLines,
          "e/F39: every brain-turn line carries cache.warm=true from the probe");
    check(deny >= 2 && nudge >= 4 && verifyFail >= 7,
          "stress ledger: DENY + nudge + verify-fail lines all recorded");
  }

  ::remove(heatPath.c_str());
  ::rmdir(heatDir.c_str());
  brainEng.stop();
  healthy.stop();

  std::cout << (failures == 0 ? "G4STRESS PASS\n" : "G4STRESS FAIL\n");
  return failures == 0 ? 0 : 1;
}
