// g4-run.cpp — Gap 4 driver: live uncapped agent turns against a local
// `coli serve` engine through the FULL host-enforced C++ stack:
//   ModelRouter (role->endpoint/model-id, fallback, velocity floor)
//   + BrainLoop (compaction F28, nudge lineages F29)
//   + G2.4 solicitToolPayload (grammar-constrained leaf proposals, F45:
//     strict parse + gate stay the enforcement on ANY engine)
//   + runGatedTask (payload gate -> spawn -> exit-code verify -> I6/I7)
//   + LedgerWriter v2 (socket-boundary tok/s, ttft, DENY/nudge/verify).
//
// Zero external network (G4.2): loopback engine only; remote-judge keys
// asserted absent (F9). Ledger path: /tmp/g4-ledger.jsonl (bench law:
// never prod state). Run tags: g4g.* (F55) — separable from earlier
// g4.* live runs and g4s.* stub-stress lines in the shared ledger.
//
// G2.4 A/B (F53/F54): every turn runs BOTH leaf-solicitation arms on
// the same engine with the same prompt —
//   baseline: plain post() -> parseStrictPayload
//   grammar : solicitToolPayload (response_format json_schema on the
//             wire; on families without grammar_payload the F46/F51
//             typed refusal triggers the unconstrained fallback, so on
//             e.g. OLMoE both arms measure strict-parse+retry honestly —
//             the summary reports which arm actually got grammar).
// Each arm retries up to kMaxNudgeDepth (3) attempts; format nudges are
// counted per arm and emitted as tagged ledger nudge lines. Parsed
// payloads then flow into runGatedTask — the gate receives structured
// JSON directly from the (grammar-constrained) stream, never prose.
//
// Usage: g4-run <brain-url> <brain-model-id> <leaf-url> <leaf-model-id>
//               [leaf2-url leaf2-model-id] [turns]
// F2 law: the brain endpoint may never sit in a leaf pool, so a valid
// run needs two distinct endpoints (one engine may serve twice on two
// ports — same family trips the G1.2 warning, which this driver
// surfaces honestly rather than hiding).
// G1.2 dual-family mode: pass leaf2 from a DIFFERENT family (e.g.
// brain=olmoe, leaf=glm-5.2-colibri, leaf2=olmoe on a second port) and
// the worker/verifier pools each hold two families — the single-family
// warnings clear and grammar-constrained solicits can fall through to
// whichever family supports grammar_payload (F46/F60).
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/grammar.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/router.hpp"
#include "dshlite/usage_probe.hpp"

namespace {
// Router-backed poster (F52: role-parameterized — brain turns route to
// the Brain pool, leaf solicitations to the Worker pool; role separation
// is never crossed). Emits one v2 `report` ledger line per routed call
// tagged "<tag>.turn<N>" (G4.1 execution trace) via fromRouted —
// latency/ttft/usage captured at the socket boundary.
struct RouterPoster : dshlite::ILlmPoster {
  dshlite::ModelRouter& router;
  dshlite::LedgerWriter* ledger = nullptr;
  /// P1 heat file (F39): when non-empty, each report line carries a
  /// fresh probe's cache block. Telemetry only — never gates (F5).
  std::string usagePath;
  dshlite::Role role = dshlite::Role::Brain;  ///< F52: pool pinning
  std::string roleName = "brain";             ///< ledger role field
  std::string tag = "g4g";                    ///< F55 task-id prefix
  long turnNo = 0;
  /// Constrained calls that SUCCEEDED (200 through the grammar path).
  /// F54/F59 honesty: a call the engine REFUSED (typed grammar
  /// rejection => solicit's unconstrained fallback) must NOT count —
  /// counting wire-attempts would claim "grammar reached the engine"
  /// when the reply actually came from the fallback.
  long constrainedServed = 0;
  explicit RouterPoster(dshlite::ModelRouter& r) : router(r) {}

  dshlite::LlmResponse post(const std::vector<dshlite::Message>& m) override {
    return route(m, nullptr);
  }
  dshlite::LlmResponse postConstrained(
      const std::vector<dshlite::Message>& m,
      const dshlite::ResponseFormat& rf) override {
    return route(m, &rf);
  }

 private:
  dshlite::LlmResponse route(const std::vector<dshlite::Message>& m,
                             const dshlite::ResponseFormat* rf) {
    auto rr = rf ? router.postConstrained(role, m, *rf) : router.post(role, m);
    // Counted only on success: a GrammarUnsupportedError throws before
    // this line, so refused grammars never inflate the count (F59).
    if (rf && !rf->empty()) ++constrainedServed;
    if (ledger) {
      ++turnNo;
      auto ev = dshlite::LedgerWriter::fromRouted(
          "report", roleName, tag + ".turn" + std::to_string(turnNo), 0, rr);
      // F33/F34: usage comes from the engine's include_usage chunk when
      // the gateway honors it (authoritative); otherwise a flagged
      // delta-count estimate (tokens_estimated in the cost block).
      ev.detail = "attempts=" + std::to_string(rr.attempts.size());
      if (!rr.attempts.empty())
        ev.detail += " last=" + rr.attempts.back().outcome;
      if (rf && !rf->empty()) ev.detail += " grammar=" + rf->type;
      if (!usagePath.empty()) {
        // Fresh probe per turn (F37: never throws, never locks; the
        // engine publishes via temp+rename so this is always a whole
        // snapshot or nothing).
        dshlite::attachCacheHeat(ev, dshlite::probeUsageFile(usagePath));
      }
      ledger->append(ev);
    }
    return rr.response;
  }
};

// One solicitation arm result (F53: bounded retry, counted nudges).
struct ArmResult {
  std::string name;             ///< "baseline" | "grammar"
  bool gotGrammar = false;      ///< constrained call reached a capable engine
  int attempts = 0;             ///< solicit calls made (>=1)
  int formatNudges = 0;         ///< strict-parse failures (retries)
  bool ok = false;              ///< final payload parsed
  nlohmann::json payload;       ///< parsed {"tool","args"} when ok
  std::string lastRaw;          ///< last verbatim content (evidence)
};

// F53: bounded solicit-retry loop — the runner owns retry (the module
// never loops by law). Cap = kMaxNudgeDepth; each format failure emits a
// tagged nudge line and counts toward the arm's formatNudges.
// F57: retries are NUDGES, not repeats — the strict-parse failure text
// is fed back into the next prompt ("your previous reply was rejected
// because X; reply with ONLY raw JSON"). Re-sending an identical prompt
// against a deterministic failure mode (e.g. OLMoE's ```json fence
// habit) would burn the cap with zero information — that is a retry
// loop defect, not a model property.
ArmResult runArm(const std::string& name, dshlite::BrainLoop& loop,
                 const std::string& prompt,
                 const std::vector<dshlite::ToolSchema>& tools,
                 dshlite::LedgerWriter& ledger, const std::string& taskId,
                 RouterPoster& poster) {
  ArmResult a;
  a.name = name;
  const bool useGrammar = (name == "grammar");
  // F59: capture this arm's starting served-count so gotGrammar reflects
  // whether THIS arm's constrained call was actually served (200), not a
  // leftover from a prior arm. solicitToolPayload swallows the typed
  // refusal and falls back to plain post, so a refused grammar leaves
  // constrainedServed unchanged => gotGrammar stays false (honest).
  const long csBefore = poster.constrainedServed;
  std::string effectivePrompt = prompt;
  std::string lastFormatErr;
  while (a.attempts < dshlite::kMaxNudgeDepth) {
    ++a.attempts;
    if (useGrammar) {
      // solicitToolPayload: constrained post -> strict parse; on a
      // grammar-incapable pool the typed refusal (F46/F51) triggers its
      // internal unconstrained fallback — gotGrammar stays false then.
      auto sr = loop.solicitToolPayload(effectivePrompt, tools);
      a.lastRaw = sr.raw;
      a.gotGrammar = poster.constrainedServed > csBefore;
      if (sr.ok) {
        a.ok = true;
        a.payload = sr.payload;
        return a;
      }
      ++a.formatNudges;
      lastFormatErr = sr.formatError;
    } else {
      // Baseline: plain post -> strict parse (no grammar on the wire).
      // Same leaf engine + same single-user-message shape solicit uses
      // internally (F52: poster is Worker-pinned), minus response_format.
      dshlite::LlmResponse r;
      try {
        r = poster.post({{"user", effectivePrompt}});
      } catch (const std::exception& e) {
        a.lastRaw = std::string("post threw: ") + e.what();
        ++a.formatNudges;
        lastFormatErr = a.lastRaw;
        dshlite::LedgerEvent ev;
        ev.type = "nudge"; ev.role = "worker"; ev.taskId = taskId;
        ev.nudgeDepth = a.formatNudges;
        ev.detail = "arm=baseline post threw: " + std::string(e.what());
        ledger.append(ev);
        continue;
      }
      a.lastRaw = r.content;
      try {
        a.payload = dshlite::parseStrictPayload(r.content);
        a.ok = true;
        return a;
      } catch (const dshlite::PayloadFormatError& e) {
        ++a.formatNudges;
        lastFormatErr = e.what();
      }
    }
    // Format failure => tagged nudge line (evidence in the ledger).
    dshlite::LedgerEvent ev;
    ev.type = "nudge";
    ev.role = "worker";
    ev.taskId = taskId;
    ev.nudgeDepth = a.formatNudges;
    ev.detail = "arm=" + a.name + " strict-parse fail attempt " +
                std::to_string(a.attempts) + "/" +
                std::to_string(dshlite::kMaxNudgeDepth) + ": " + lastFormatErr;
    ledger.append(ev);
    // F57: feed the rejection reason back into the next attempt, and
    // F58: re-show the bare unfenced example (surface-form imitation).
    effectivePrompt = prompt +
        "\n\nYour previous reply was REJECTED by the strict JSON parser: " +
        lastFormatErr +
        "\nReply again with ONLY the raw JSON object, exactly like this "
        "example line (no fences, no prose):\n"
        "{\"tool\":\"shell\",\"args\":{\"cmd\":\"echo hello\"}}";
  }
  return a;
}
}  // namespace

int main(int argc, char** argv) {
  using namespace dshlite;
  if (argc < 5) {
    std::cerr << "usage: g4-run <brain-url> <brain-model-id> "
                 "<leaf-url> <leaf-model-id> [leaf2-url leaf2-model-id] [turns]\n";
    return 2;
  }
  const std::string brainUrl = argv[1];
  const std::string brainModel = argv[2];
  const std::string leafUrl = argv[3];
  const std::string leafModel = argv[4];
  // Optional second leaf (dual-family mode, G1.2). Disambiguate the
  // trailing [turns] int from a URL positional.
  std::string leaf2Url, leaf2Model;
  int turns = 3;
  if (argc > 5) {
    const std::string a5 = argv[5];
    if (a5.rfind("http", 0) == 0 && argc > 6) {
      leaf2Url = a5;
      leaf2Model = argv[6];
      if (argc > 7) turns = std::atoi(argv[7]);
    } else {
      turns = std::atoi(argv[5]);
    }
  }
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
  if (leaf2Url.empty()) {
    rc.worker = {makeEntry(leafUrl, leafModel)};     // F2: disjoint endpoint
    rc.verifier = {makeEntry(leafUrl, leafModel)};
  } else {
    // G1.2 dual-family: two leaf entries per pool, opposite orders so a
    // fallthrough exercises the second family, not the same engine.
    rc.worker = {makeEntry(leafUrl, leafModel), makeEntry(leaf2Url, leaf2Model)};
    rc.verifier = {makeEntry(leaf2Url, leaf2Model), makeEntry(leafUrl, leafModel)};
  }
  ModelRouter router(rc);
  for (const auto& w : router.warnings())
    std::cout << "[warn] " << w << "\n";
  if (router.warnings().empty())
    std::cout << "[ok] G1.2: no single-family warnings — heterogeneous pools registered\n";

  // F60 pre-warn: which leaf models can actually serve grammar drafts.
  // Informational only — the gateway's typed 400 stays authoritative.
  for (const auto& m : {leafModel, leaf2Model}) {
    if (m.empty()) continue;
    std::cout << "[grammar] " << m << " -> grammar_payload="
              << (modelSupportsGrammar(m) ? "yes" : "no") << "\n";
  }

  // Pre-flight probe (G1.1): engine must answer its own model-id.
  for (const auto& p : router.probe()) {
    std::cout << "[probe] " << p.modelId << " @ " << p.endpoint << " -> "
              << (p.ok ? "OK" : "FAIL: " + p.detail) << "\n";
    if (!p.ok) return 4;
  }

  // F39: heat wiring is opt-in via COLI_USAGE (same env var the engine
  // itself reads). Empty => no probe, no cache block (F5 best-effort).
  const char* usageEnv = std::getenv("COLI_USAGE");

  // Brain loop: executive turns through the Brain pool (F52).
  RouterPoster brainPoster(router);
  brainPoster.ledger = &ledger;
  brainPoster.role = Role::Brain;
  brainPoster.roleName = "brain";
  brainPoster.tag = "g4g.brain";
  if (usageEnv) brainPoster.usagePath = usageEnv;
  BrainLoop brain(brainPoster, [](const std::string& c) -> JudgeVerdict {
    JudgeVerdict v;
    v.route = "do_direct";
    v.confidence = 0.9;
    v.action = c.size() < 80 ? JudgeAction::DoDirect : JudgeAction::SplitOnce;
    v.detail = "local heuristic judge";
    return v;
  });
  BrainLoop::HostConfig bhc;
  bhc.policy.allowedTools = {"shell"};
  bhc.ledger = &ledger;
  brain.setHostConfig(std::move(bhc));
  brain.setSystemPrompt(
      "You are the Budget-AGI brain. Answer in one or two sentences.");

  // Leaf loop: tool-payload solicitation through the WORKER pool (F52).
  // Its own poster so leaf report lines are tagged/role-stamped as leaf
  // and never mix with brain telemetry.
  RouterPoster leafPoster(router);
  leafPoster.ledger = &ledger;
  leafPoster.role = Role::Worker;
  leafPoster.roleName = "worker";
  leafPoster.tag = "g4g.leaf";
  if (usageEnv) leafPoster.usagePath = usageEnv;
  BrainLoop leaf(leafPoster, [](const std::string&) -> JudgeVerdict {
    JudgeVerdict v;
    v.action = JudgeAction::DoDirect;
    v.route = "do_direct";
    v.confidence = 0.9;
    return v;
  });
  BrainLoop::HostConfig lhc;
  lhc.policy.allowedTools = {"shell"};
  lhc.ledger = &ledger;
  leaf.setHostConfig(std::move(lhc));

  // G2.4 tool registry: one allowed tool, schema inside the engine
  // compiler's fail-closed subset (F49). Single tool => args constrained.
  const std::vector<ToolSchema> tools = {
      {"shell",
       {{"type", "object"},
        {"properties", {{"cmd", {{"type", "string"}}}}},
        {"required", {"cmd"}}}}};

  // Ledger baseline so the delta summary counts only THIS run's lines.
  long ledgerLinesBefore = 0;
  {
    std::ifstream in("/tmp/g4-ledger.jsonl");
    std::string ln;
    while (std::getline(in, ln))
      if (!ln.empty()) ++ledgerLinesBefore;
  }

  // F58 (live finding): OLMoE fences whatever shape the prompt's last
  // example showed — an inline schema in prose elicited ```json fences
  // on 6/6 attempts (nudge text included), while a BARE unfenced
  // example line elicited raw JSON first-try. The model imitates
  // surface form, so the prompt must model the surface form.
  const std::string solicitPrompt =
      "Propose one shell tool call that echoes the word atom.\n"
      "Example of the exact expected reply format:\n"
      "{\"tool\":\"shell\",\"args\":{\"cmd\":\"echo hello\"}}\n"
      "Now propose the call for echoing atom. Reply with only the JSON object.";

  int baselineNudges = 0, grammarNudges = 0;
  int baselineOk = 0, grammarOk = 0;
  bool grammarEverReachedCapable = false;
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

    // G2.4 A/B: both arms solicit a tool payload from the leaf engine.
    const std::string baseTid = "g4g.t" + std::to_string(i + 1) + ".base";
    const std::string gramTid = "g4g.t" + std::to_string(i + 1) + ".gram";
    ArmResult base = runArm("baseline", leaf, solicitPrompt, tools, ledger,
                            baseTid, leafPoster);
    ArmResult gram = runArm("grammar", leaf, solicitPrompt, tools, ledger,
                            gramTid, leafPoster);
    // F59: gotGrammar is computed per-arm inside runArm from the served
    // delta — true only if THIS arm's constrained call was actually
    // served (200), false if the engine refused and solicit fell back.
    grammarEverReachedCapable = grammarEverReachedCapable || gram.gotGrammar;

    baselineNudges += base.formatNudges;
    grammarNudges += gram.formatNudges;
    baselineOk += base.ok ? 1 : 0;
    grammarOk += gram.ok ? 1 : 0;
    std::cout << "[solicit base] ok=" << base.ok << " attempts=" << base.attempts
              << " nudges=" << base.formatNudges << "\n"
              << "[solicit gram] ok=" << gram.ok << " attempts=" << gram.attempts
              << " nudges=" << gram.formatNudges
              << " grammarReachedCapable=" << gram.gotGrammar << "\n";

    // Gate receives the parsed payload directly (objective 1.2): run the
    // gated task on whichever arm parsed (prefer grammar arm). Prose never
    // reaches the gate — a failed solicit means no spawn at all.
    if (gram.ok || base.ok) {
      const nlohmann::json& payload = gram.ok ? gram.payload : base.payload;
      SpawnOptions opt;
      opt.argv = {"/bin/sh", "-c", "echo atom-done; exit 0"};
      opt.timeout = std::chrono::seconds(30);
      auto rep = brain.runGatedTask("g4g.t" + std::to_string(i + 1), payload, opt);
      std::cout << "[task g4g.t" << i + 1 << "] disposition=" << rep.disposition
                << " spawns=" << rep.spawns
                << " verify=" << (rep.verify.pass ? "pass" : "fail") << "\n";
    } else {
      std::cout << "[task g4g.t" << i + 1
                << "] SKIPPED — neither arm parsed strict JSON (gate never saw prose)\n";
    }
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
  // G2.4 A/B delta (F54 — reported honestly, not asserted to zero):
  std::cout << "--- G2.4 solicitation A/B (leaf tool calls) ---\n"
            << "baseline: ok=" << baselineOk << "/" << turns
            << " format_nudges=" << baselineNudges << "\n"
            << "grammar : ok=" << grammarOk << "/" << turns
            << " format_nudges=" << grammarNudges
            << " grammar_reached_capable_engine="
            << (grammarEverReachedCapable ? "yes" : "no") << "\n";
  if (!grammarEverReachedCapable)
    std::cout << "[note] leaf family lacks grammar_payload (F46): grammar arm "
                 "ran the unconstrained fallback — nudge delta reflects "
                 "strict-parse+retry only, NOT grammar acceleration (F54)\n";

  // Ledger delta summary (F55): count this run's tagged lines by type.
  long newLines = 0, nudge = 0, report = 0, deny = 0, verify = 0, warm = 0;
  {
    std::ifstream in("/tmp/g4-ledger.jsonl");
    std::string ln;
    long idx = 0;
    while (std::getline(in, ln)) {
      if (ln.empty()) continue;
      if (idx++ < ledgerLinesBefore) continue;  // only THIS run's delta
      ++newLines;
      const auto j = nlohmann::json::parse(ln);
      const std::string t = j.value("type", "");
      if (t == "nudge") ++nudge;
      else if (t == "report") ++report;
      else if (t == "DENY") ++deny;
      else if (t == "verify") ++verify;
      if (j.contains("cache") && j["cache"].value("warm", false)) ++warm;
    }
  }
  std::cout << "--- ledger delta (this run, g4g.*) ---\n"
            << "new_lines=" << newLines << " report=" << report
            << " nudge=" << nudge << " verify=" << verify << " DENY=" << deny
            << " warm_cache=" << warm << "\n";
  std::cout << "ledger: /tmp/g4-ledger.jsonl\n";
  return 0;
}
