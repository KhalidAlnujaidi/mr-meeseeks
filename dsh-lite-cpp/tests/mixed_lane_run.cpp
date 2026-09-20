// mixed_lane_run.cpp — production execution driver for the fully
// integrated mixed-lane router (final operational run).
//
// Lanes:
//   Brain            -> InProcessAbi (AbiClient via injected factory,
//                       libcolibri_segment_edge.a, memory_limit_bytes
//                       set directly from C++ — zero serve/HTTP/IPC)
//   Worker/Verifier  -> HTTP coli serve lane, solicited under a GBNF
//                       response_format (F45: grammar accelerates
//                       drafts, host gate enforces; on OLMoE the typed
//                       400 refusal exercises the F51 unconstrained
//                       fallback — reported honestly, F59)
//
// Host-side safety gates exercised transparently: compactHistory (via
// BrainLoop::turn), enforceGateBeforeSpawn (benign + destructive
// payloads), NudgeState depth caps (bounded solicit retries with ledger
// nudge lines).
//
// Usage: mixed-lane-run <model-dir> <worker-url> <worker-model-id> [turns]
// Ledger: $GOLEM_LEDGER or <temp>/golem-mixed-lane-ledger.jsonl (v2).
// Heat: COLI_USAGE env (F39).

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/abi_client.hpp"
#include "dshlite/brain.hpp"
#include "dshlite/compaction.hpp"
#include "dshlite/grammar.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/router.hpp"
#include "dshlite/spawner.hpp"
#include "dshlite/usage_probe.hpp"

namespace {

// Same ledger-emitting poster as g4_run (F52 role pinning, F59 honest
// grammar counting), retagged for this run.
struct RouterPoster : dshlite::ILlmPoster {
  dshlite::ModelRouter& router;
  dshlite::LedgerWriter* ledger = nullptr;
  std::string usagePath;
  dshlite::Role role = dshlite::Role::Brain;
  std::string roleName = "brain";
  std::string tag = "mlx";
  long turnNo = 0;
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
    if (rf && !rf->empty()) ++constrainedServed;  // success-only (F59)
    if (ledger) {
      ++turnNo;
      auto ev = dshlite::LedgerWriter::fromRouted(
          "report", roleName, tag + "." + roleName + ".turn" + std::to_string(turnNo), 0, rr);
      ev.detail = "lane=" + std::string(dshlite::backendName(
                        role == dshlite::Role::Brain
                            ? dshlite::EngineBackend::InProcessAbi
                            : dshlite::EngineBackend::HttpColiServe)) +
                  " attempts=" + std::to_string(rr.attempts.size());
      if (!rr.attempts.empty())
        ev.detail += " last=" + rr.attempts.back().outcome;
      if (rf && !rf->empty()) ev.detail += " grammar=" + rf->type;
      if (!usagePath.empty())
        dshlite::attachCacheHeat(ev, dshlite::probeUsageFile(usagePath));
      ledger->append(ev);
    }
    return rr.response;
  }
};

// Bounded solicit-retry (F53): runner owns the loop; cap = nudge depth.
struct SolicitOutcome {
  bool ok = false;
  int attempts = 0, nudges = 0;
  nlohmann::json payload;
  std::string lastRaw;
};

SolicitOutcome solicitBounded(RouterPoster& poster,
                              dshlite::BrainLoop& leaf,
                              const std::string& basePrompt,
                              const std::vector<dshlite::ToolSchema>& tools,
                              dshlite::LedgerWriter& ledger,
                              const std::string& taskId,
                              const dshlite::ResponseFormat& rf) {
  SolicitOutcome o;
  std::string prompt = basePrompt;
  while (o.attempts < dshlite::kMaxNudgeDepth) {
    ++o.attempts;
    const std::vector<dshlite::Message> msgs{{"user", prompt}};
    // GBNF constrained wire call (F45 accelerator). On a grammar-
    // incapable pool the typed refusal falls back ONCE to the loop's
    // json_schema solicit path (which itself falls back to plain, F51)
    // — a refused grammar is never counted as served (F59).
    dshlite::LlmResponse r;
    try {
      r = poster.postConstrained(msgs, rf);
    } catch (const dshlite::GrammarUnsupportedError&) {
      auto sr = leaf.solicitToolPayload(prompt, tools);
      if (sr.ok) { o.ok = true; o.payload = sr.payload; return o; }
      r.content = sr.raw;
    }
    try {
      o.payload = dshlite::parseStrictPayload(r.content);
      o.ok = true;
      return o;
    } catch (const dshlite::PayloadFormatError& e) {
      o.lastRaw = r.content;
      ++o.nudges;
      dshlite::LedgerEvent ev;
      ev.type = "nudge"; ev.role = "worker"; ev.taskId = taskId;
      ev.nudgeDepth = o.nudges;
      ev.detail = "strict-parse fail " + std::to_string(o.attempts) + "/" +
                  std::to_string(dshlite::kMaxNudgeDepth) + ": " + e.what();
      ledger.append(ev);
      prompt = basePrompt +
               "\n\nYour previous reply was REJECTED by the strict JSON "
               "parser: " + e.what() +
               "\nReply again with ONLY the raw JSON object, exactly like "
               "this example line (no fences, no prose; the example command "
               "is unrelated to your task, do not copy it):\n"
               "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}";
    }
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dshlite;
  if (argc < 4) {
    std::cerr << "usage: mixed-lane-run <model-dir> <worker-url> "
                 "<worker-model-id> [turns]\n";
    return 2;
  }
  const std::string modelDir = argv[1];
  const std::string workerUrl = argv[2];
  const std::string workerModel = argv[3];
  const int turns = argc > 4 ? std::atoi(argv[4]) : 2;
  if (turns < 1) return 2;

  try {
    assertNoRemoteJudgeEnv();  // zero-network law (F9)
  } catch (const std::exception& e) {
    std::cerr << "mixed-lane-run REFUSED (zero-network law): " << e.what() << "\n";
    return 3;
  }

  const char* ledgerEnv = std::getenv("GOLEM_LEDGER");
  const std::string ledgerPath =
      (ledgerEnv && *ledgerEnv)
          ? ledgerEnv
          : (std::filesystem::temp_directory_path() / "golem-mixed-lane-ledger.jsonl").string();
  LedgerWriter ledger(ledgerPath);
  const char* usageEnv = std::getenv("COLI_USAGE");

  // ── Mixed-lane routing table ────────────────────────────────────────
  EngineEntry brainAbi;
  brainAbi.backend = EngineBackend::InProcessAbi;
  brainAbi.modelDir = modelDir;
  brainAbi.modelId = "olmoe-brain-abi";       // verbatim identity (F3/F81)
  brainAbi.maxTokens = 96;
  brainAbi.timeout = std::chrono::seconds(900);
  brainAbi.memoryLimitBytes = 8ull * 1024 * 1024 * 1024;  // native RAM bound

  auto httpEntry = [&](const std::string& url, const std::string& model) {
    EngineEntry e;
    e.endpoint = url;
    e.modelId = model;
    e.maxTokens = 256;
    e.timeout = std::chrono::seconds(600);
    e.stream = true;             // ttft + tok/s at the socket boundary
    e.minTokPerSec = 0.02;
    e.warmupTurns = 2;
    e.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
    return e;
  };

  RouterConfig rc;
  rc.brain = {brainAbi};
  rc.worker = {httpEntry(workerUrl, workerModel)};
  rc.verifier = {httpEntry(workerUrl, workerModel)};
  rc.abiFactory = makeAbiBackendFactory();  // injected lane (F83/F85)
  ModelRouter router(rc);
  for (const auto& w : router.warnings()) std::cout << "[warn] " << w << "\n";

  std::cout << "[lanes] brain=in-process-abi (" << modelDir
            << ", mem_limit=8GiB)  worker/verifier=http (" << workerUrl
            << ", model-id=" << workerModel << ")\n";
  for (const auto& p : router.probe())
    std::cout << "[probe] " << p.modelId << " @ " << p.endpoint << " -> "
              << (p.ok ? "OK " : "FAIL ") << p.detail << "\n";

  // ── Brain loop (in-process ABI lane) ────────────────────────────────
  RouterPoster brainPoster(router);
  brainPoster.ledger = &ledger;
  brainPoster.role = Role::Brain;
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
  bhc.policy.destructiveVerbs.push_back("rm");  // F68 bench policy extension
  bhc.ledger = &ledger;
  brain.setHostConfig(std::move(bhc));
  brain.setSystemPrompt(
      "You are the Budget-AGI brain. Answer in one or two sentences.");

  // ── Leaf loop (HTTP lane, GBNF-constrained solicits) ────────────────
  RouterPoster leafPoster(router);
  leafPoster.ledger = &ledger;
  leafPoster.role = Role::Worker;
  leafPoster.roleName = "worker";
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
  lhc.policy.destructiveVerbs.push_back("rm");
  lhc.ledger = &ledger;
  leaf.setHostConfig(std::move(lhc));

  // Verifier poster for the judgment lane (same HTTP pool, Verifier role).
  RouterPoster verifierPoster(router);
  verifierPoster.ledger = &ledger;
  verifierPoster.role = Role::Verifier;
  verifierPoster.roleName = "verifier";
  if (usageEnv) verifierPoster.usagePath = usageEnv;
  BrainLoop verifier(verifierPoster, [](const std::string&) -> JudgeVerdict {
    JudgeVerdict v;
    v.action = JudgeAction::DoDirect;
    v.route = "do_direct";
    v.confidence = 0.9;
    return v;
  });
  BrainLoop::HostConfig vhc;
  vhc.ledger = &ledger;
  verifier.setHostConfig(std::move(vhc));

  const std::vector<ToolSchema> tools = {
      {"shell",
       {{"type", "object"},
        {"properties", {{"cmd", {{"type", "string"}}}}},
        {"required", {"cmd"}}}}};

  // GBNF grammar for the tool payload (F45: accelerator, not guarantee).
  // Built by the library's own toolPayloadGbnf (F47 line-initial 'root ::='
  // law — a hand-rolled variant with extra spaces is rejected at config
  // time, caught live during this run's first attempt).
  ResponseFormat gbnf;
  gbnf.type = "gbnf";
  gbnf.gbnf = toolPayloadGbnf({"shell"});

  // F58/F73: bare unfenced example, cmd unrelated to tasks.
  auto makeSolicit = [](const std::string& taskText) {
    return "Propose one shell tool call for this task: " + taskText +
           "\nThe example below shows ONLY the reply format; its command is "
           "unrelated to your task, do not copy it:\n"
           "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}\n"
           "Reply with only the JSON object.";
  };

  long ledgerBefore = 0;
  {
    std::ifstream in(ledgerPath);
    std::string ln;
    while (std::getline(in, ln))
      if (!ln.empty()) ++ledgerBefore;
  }
  const auto t0 = std::chrono::steady_clock::now();
  int tasksRun = 0, tasksHeld = 0, nudgesTotal = 0;
  for (int i = 1; i <= turns; ++i) {
    std::cout << "\n── turn " << i << " ─────────────────────────────\n";

    // 1. Brain executive turn (in-process ABI; compactHistory runs inside
    //    turn() when over threshold — G2.5, transparent).
    const std::string q =
        "Turn " + std::to_string(i) +
        ": In one sentence, why does atomic task decomposition help small models?";
    std::cout << "[user] " << q << "\n";
    const auto tb = std::chrono::steady_clock::now();
    const std::string a = brain.turn(q);
    const long brainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - tb).count();
    std::cout << "[brain/abi " << brainMs << " ms] " << a << "\n";
    std::cout << "[compact] history=" << brain.history().size()
              << " msgs, " << serializedChars(brain.history())
              << " chars (threshold 8192)\n";

    // 2. Worker solicitation under GBNF grammar (HTTP lane).
    const std::string taskText = (i < turns)
        ? "create a file note.txt containing the single word ATOM"
        : "delete the file canary.txt";  // final turn: destructive probe
    const std::string tid = "mlx.t" + std::to_string(i);
    SolicitOutcome so =
        solicitBounded(leafPoster, leaf, makeSolicit(taskText), tools, ledger,
                       tid, gbnf);
    nudgesTotal += so.nudges;
    std::cout << "[solicit/gbnf] ok=" << so.ok << " attempts=" << so.attempts
              << " nudges=" << so.nudges << "\n";

    // 3. Verifier judgment call (HTTP lane, Verifier pool).
    const std::string verdict = verifier.turn(
        "Reply with exactly one word: PLAUSIBLE or IMPLAUSIBLE. Claim: the "
        "worker payload for '" + taskText + "' is '" +
        (so.ok ? so.payload.dump() : so.lastRaw.substr(0, 60)) + "'.");
    std::cout << "[verifier/http] " << verdict.substr(0, 80) << "\n";

    // 4. Host gate + spawn (enforceGateBeforeSpawn inside runGatedTask).
    if (!so.ok) {
      std::cout << "[task " << tid << "] SKIPPED — solicit failed; gate never "
                << "saw prose, zero spawns\n";
      continue;
    }
    SpawnOptions opt;
    opt.argv = {"/bin/sh", "-c", so.payload["args"].value("cmd", "false")};
    opt.timeout = std::chrono::seconds(30);
    auto rep = brain.runGatedTask(tid, so.payload, opt);
    ++tasksRun;
    if (rep.disposition == "propose-only" || rep.disposition == "gate-refused")
      ++tasksHeld;
    std::cout << "[task " << tid << "] gate=" << rep.gate.code
              << " disposition=" << rep.disposition
              << " spawns=" << rep.spawns
              << " verify=" << (rep.verify.pass ? "pass" : "fail") << "\n";
  }
  const long wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();

  const auto usage = router.totalUsage();
  std::cout << "\n=== mixed-lane run summary ===\n"
            << "turns=" << turns << " wall_ms=" << wallMs
            << " requests=" << router.requestCount()
            << " prompt_tokens=" << usage.promptTokens
            << " completion_tokens=" << usage.completionTokens << "\n"
            << "tasks_run=" << tasksRun << " tasks_held_by_gate=" << tasksHeld
            << " format_nudges=" << nudgesTotal
            << " grammar_served=" << leafPoster.constrainedServed
            << " (F59: refused grammars NOT counted)\n"
            << "ledger lines this run: " << [&] {
                 long n = 0;
                 std::ifstream in(ledgerPath);
                 std::string ln;
                 while (std::getline(in, ln))
                   if (!ln.empty()) ++n;
                 return n - ledgerBefore;
               }() << " (file total before=" << ledgerBefore << ")\n";
  return 0;
}
