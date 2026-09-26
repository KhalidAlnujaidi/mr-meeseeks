// golem_runner.cpp — Golem adapter for the Iso-Model Harness Benchmark
// (bench/iso_harness). Runs ONE task (or a csv subset) through the real
// Golem execution contract: strict-parse solicit -> host payload gate ->
// sandboxed spawn -> sanitized result. Fixtures are pre-seeded by the
// orchestrator; this runner copies scope files into the ephemeral worker
// workspace and copies resulting artifacts back (F88: makes the fresh-
// workspace contract observable to the shared-sandbox referee without
// weakening isolation).
//
// Verdicts are NOT made here (F89): this writes a diagnostic out.json
// (exit codes, dispositions, timing window); referee.py decides pass_f72
// from the filesystem + proxy log.
//
// usage: golem-runner <proxy-url> <model-id> <sandbox-dir> <tasks.json>
//                     <out.json> <task-ids-csv>
// F73: prompt example is task-unrelated and marked do-not-copy.

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/llm_client.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/payload_gate.hpp"
#include "dshlite/postcondition.hpp"
#include "dshlite/spawner.hpp"

namespace fs = std::filesystem;
using namespace dshlite;
using json = nlohmann::json;

namespace {

void note(const std::string& s) { std::cerr << "[golem-iso] " << s << "\n"; }

std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Peak RSS of THIS process in MB (F90: adapter-process memory).
long peakRssMb() {
  struct rusage ru {};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return ru.ru_maxrss / (1024 * 1024);  // bytes on macOS
#else
  return ru.ru_maxrss / 1024;  // KB on Linux
#endif
}

// Poster that also accumulates usage like the h2h BenchPoster.
struct BenchPoster : ILlmPoster {
  BenchPoster(LlmClient& c, LedgerWriter& l, std::string model, std::string ep)
      : client(c), ledger(l), modelId(std::move(model)),
        endpoint(std::move(ep)) {}
  LlmClient& client;
  LedgerWriter& ledger;
  std::string modelId, endpoint;

  LlmResponse post(const std::vector<Message>& messages) override {
    LlmResponse r = client.post(messages);
    LedgerEvent ev;
    ev.type = "report";
    ev.role = "worker";
    ev.model = modelId;
    ev.endpoint = endpoint;
    ev.promptTokens = r.usage.promptTokens;
    ev.completionTokens = r.usage.completionTokens;
    ev.latencyMs = r.latencyMs;
    ev.ttftMs = r.ttftMs;
    ev.tokensEstimated = r.usageEstimated;  // F35 provenance
    ev.detail = "lane=http-coli-serve iso-bench";
    ledger.append(ev);
    return r;
  }
};

std::vector<std::string> splitCsv(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ','))
    if (!item.empty()) out.push_back(item);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 7) {
    std::cerr << "usage: golem-runner <proxy-url> <model-id> <sandbox-dir> "
                 "<tasks.json> <out.json> <task-ids-csv>\n";
    return 2;
  }
  const std::string proxyUrl = argv[1];
  const std::string modelId = argv[2];
  const std::string sandbox = argv[3];
  const json tasksDoc = json::parse(slurp(argv[4]));
  const std::string outPath = argv[5];
  const auto wanted = splitCsv(argv[6]);

  try {
    assertNoRemoteJudgeEnv();  // zero-network law (F9)
  } catch (const std::exception& e) {
    std::cerr << "golem-iso REFUSED (zero-network law): " << e.what() << "\n";
    return 3;
  }

  const fs::path sandboxDir(sandbox);
  fs::create_directories(sandboxDir);
  const std::string ledgerPath = (sandboxDir / "golem-ledger.jsonl").string();
  LedgerWriter ledger(ledgerPath);

  LlmConfig lc;
  lc.endpoint = proxyUrl;
  lc.model = modelId;
  lc.maxTokens = 256;           // iso protocol lock
  // F92: temperature=0.0 is an engine-side lock (colibri decodes
  // deterministically); LlmClient sends no temperature field — parity
  // with the h2h protocol where both arms relied on greedy decode.
  lc.timeout = std::chrono::seconds(600);
  lc.stream = true;
  lc.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";  // loopback, no key
  LlmClient client(lc);
  BenchPoster poster(client, ledger, modelId, proxyUrl);
  BrainLoop brain(poster, [](const std::string&) -> JudgeVerdict {
    JudgeVerdict v;
    v.action = JudgeAction::DoDirect;
    v.route = "do_direct";
    v.confidence = 0.9;
    return v;
  });
  BrainLoop::HostConfig hc;
  hc.policy.allowedTools = {"shell"};
  hc.policy.destructiveVerbs.push_back("rm");  // F68 bench policy extension
  hc.ledger = &ledger;
  brain.setHostConfig(std::move(hc));
  const std::vector<ToolSchema> tools = {
      {"shell",
       {{"type", "object"},
        {"properties", {{"cmd", {{"type", "string"}}}}},
        {"required", {"cmd"}}}}};

  auto makePrompt = [](const std::string& taskText, int round, int maxRounds) {
    std::string p = "Propose ONE shell tool call for this task: " + taskText;
    if (maxRounds > 1)
      p += "\nThis is step " + std::to_string(round) + " of at most " +
           std::to_string(maxRounds) +
           "; the tool result of the previous step (if any) is shown above.";
    p += "\nThe example below shows ONLY the reply format; its command"
         " is unrelated to your task, do not copy it:\n"
         "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}\n"
         "Reply with only the JSON object.";
    return p;
  };

  // Warmup only when the orchestrator says so (first invocation per arm,
  // F66/F93 parity — one warmup per harness, proxy-logged, unjudged).
  if (const char* w = std::getenv("ISO_WARMUP"); w && std::string(w) == "1") {
    try {
      (void)client.post({{"user", "Reply with the single word: warm"}});
    } catch (const std::exception& e) {
      note(std::string("[warmup] failed (diagnostic): ") + e.what());
    }
  }

  json results = json::array();
  for (const auto& t : tasksDoc["tasks"]) {
    const std::string id = t["id"];
    if (std::find(wanted.begin(), wanted.end(), id) == wanted.end()) continue;
    const std::string text = t["text"];
    const int maxRounds = t.value("max_rounds", 1);
    note(id + ": " + text);

    json row;
    row["task"] = id;
    const auto wallStart = std::chrono::system_clock::now();
    const auto t0 = std::chrono::steady_clock::now();
    row["started_at_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wallStart.time_since_epoch())
            .count();

    std::vector<int> exitCodes;
    std::vector<std::string> dispositions;
    int solicitFails = 0, spawns = 0, gateHolds = 0, resolicits = 0;
    std::string lastOutput;
    // F72 layer 2 (promoted): the harness's own content verdict, computed
    // with the RUNTIME predicate engine. The referee re-judges the
    // sandbox independently (F89) — agreement between the two is the
    // interesting signal, disagreement is a finding.
    bool postcondEvaluated = false;
    bool postcondPass = false;
    std::string postcondDetail;

    for (int round = 1; round <= maxRounds; ++round) {
      // Bounded solicit-retry (F53): cap = nudge depth law.
      BrainLoop::SolicitResult sr;
      int attempts = 0;
      std::string prompt = makePrompt(text, round, maxRounds);
      if (!lastOutput.empty())
        prompt = "Previous step output:\n" + lastOutput + "\n\n" + prompt;
      while (attempts < kMaxNudgeDepth) {
        ++attempts;
        sr = brain.solicitToolPayload(prompt, tools);
        if (sr.ok) break;
        prompt = makePrompt(text, round, maxRounds) +
                 "\n\nYour previous reply was REJECTED by the strict JSON "
                 "parser. Reply again with ONLY the raw JSON object, "
                 "exactly like this example line (no fences, no prose; "
                 "the example command is unrelated to your task, do not "
                 "copy it):\n"
                 "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}";
      }
      if (!sr.ok) {
        ++solicitFails;
        dispositions.push_back("solicit-failed");
        note(id + ": round " + std::to_string(round) + " solicit failed");
        break;  // no payload to gate/execute
      }

      const json payload = sr.payload;
      const std::string taskId = "iso." + id;
      GateVerdict gv;
      try {
        gv = checkPayload(payload, brain.hostConfig().policy);
      } catch (const std::exception& e) {
        gv.allowed = false;
        gv.code = "GATE_EXCEPTION";
        gv.message = e.what();
      }

      if (!gv.allowed || !autoExecutable(gv)) {
        // Safety law: destructive payloads are propose-only, zero spawns.
        ++gateHolds;
        dispositions.push_back(gv.allowed ? "propose-only" : "gate-refused");
        LedgerEvent ev;
        ev.type = gv.allowed ? "report" : "DENY";
        ev.taskId = taskId;
        ev.role = "worker";
        if (!gv.allowed) { ev.gate = "SPAWN"; ev.code = gv.code; }
        else ev.verdict = "propose-only";
        ev.detail = gv.code + ": " + gv.message;
        ledger.append(ev);
        note(id + ": gate=" + gv.code + " hold (zero spawns)");
        break;  // gate held; further rounds would re-solicit the same probe
      }
      // Spawn in the ephemeral workspace; scope = sandbox files.
      SpawnOptions opt;
      opt.argv = {"/bin/sh", "-c", payload["args"].value("cmd", "false")};
      opt.timeout = std::chrono::seconds(30);
      std::error_code ec;
      for (const auto& e : fs::directory_iterator(sandboxDir, ec)) {
        if (e.is_regular_file() && e.path().filename() != "golem-ledger.jsonl")
          opt.scopeFiles.push_back(e.path().string());
      }

      // F72 layer 2 + F88, as the runtime hook: layer 2 must run IN the
      // ephemeral workspace before runGatedTask cleans it, and that same
      // window is the only chance to copy the artifacts back to the shared
      // sandbox for the referee (F89 reads the sandbox, so losing them
      // would look like a disagreement that has nothing to do with the
      // harness under test).
      BrainLoop::PostconditionHook postcond =
          [gt = t.contains("ground_truth") ? t["ground_truth"] : json(),
           &sandboxDir](const std::string& ws) {
            PostconditionResult pc = checkPostcondition(ws, gt);
            std::error_code cec;
            for (const auto& e : fs::recursive_directory_iterator(ws, cec)) {
              if (!e.is_regular_file(cec)) continue;
              fs::path rel = fs::relative(e.path(), ws, cec);
              if (cec || rel.empty() || rel.native()[0] == '.') continue;
              fs::path dest = sandboxDir / rel;
              fs::create_directories(dest.parent_path(), cec);
              fs::copy_file(e.path(), dest, fs::copy_options::overwrite_existing,
                            cec);
            }
            return pc;
          };

      // F72 part 2 (P3-b): on a verify failure the RUNTIME asks this hook
      // for a fresh payload, instead of the runner re-asking on its own
      // (which had no cap awareness). The hook owns the model call; the
      // loop owns the gate and the I6/I7 ladder.
      int hookCalls = 0;
      auto resolicit = [&](const BrainLoop::FailureFeedback& fb)
          -> std::optional<json> {
        ++hookCalls;
        // Rebuild the prompt from the task text only: no previous-round
        // framing bleeds in (F73).
        std::string rp = makePrompt(text, round, maxRounds);
        rp += "\n\nYour previous command did not produce the required result.";
        if (fb.postconditionEvaluated && !fb.postconditionOk &&
            t.contains("ground_truth"))
          rp += "\nThe result must be exactly: " + t["ground_truth"].dump();
        BrainLoop::SolicitResult ns = brain.solicitToolPayload(rp, tools);
        if (!ns.ok) {
          ++solicitFails;
          note(id + ": re-solicit parse failed (" + ns.formatError + ")");
          return std::nullopt;  // no fresh payload => runtime replays
        }
        return ns.payload;
      };

      // The host's only lever for the I6 cap law is to actually supply a
      // narrowed payload (the planner cannot rewrite argv, F20), so declare
      // NarrowOrReroute while a re-solicit is still allowed; past that, the
      // honest answer is FullScope and the runtime stops the lineage.
      constexpr int kMaxResolicits = 1;
      BrainLoop::RetryPlanner planner = [&](int, json&) -> RetryScope {
        return hookCalls < kMaxResolicits ? RetryScope::NarrowOrReroute
                                          : RetryScope::FullScope;
      };

      // F99: one definition of payload -> argv, used for the initial
      // payload AND every re-solicited one. Without this the re-solicit
      // hook would be a no-op: the loop would gate the fresh payload but
      // re-run the original command.
      auto bindExec = [](SpawnOptions& so, const json& pl) {
        so.argv = {"/bin/sh", "-c", pl["args"].value("cmd", "false")};
      };

      BrainLoop::TaskReport rep;
      try {
        rep = brain.runGatedTask(taskId, payload, opt, planner, postcond,
                                 resolicit, kMaxResolicits, bindExec);
      } catch (const std::exception& e) {
        dispositions.push_back("spawn-error");
        note(id + ": runGatedTask threw " + e.what());
        break;
      }
      spawns += rep.spawns;
      if (rep.spawns == 0) ++gateHolds;
      exitCodes.push_back(rep.verify.exitCode);
      dispositions.push_back(rep.disposition);
      resolicits += rep.resolicits;
      lastOutput = rep.verify.detail;
      postcondEvaluated = rep.postcondition.evaluated;
      postcondPass = rep.postcondition.pass;
      postcondDetail = rep.postcondition.detail;
      note(id + ": round " + std::to_string(round) + " " +
           rep.disposition + " exits=" + std::to_string(rep.verify.exitCode) +
           " resolicits=" + std::to_string(rep.resolicits));
    }

    row["exit_codes"] = exitCodes;
    row["dispositions"] = dispositions;
    row["postcond_evaluated"] = postcondEvaluated;
    row["postcond_pass"] = postcondEvaluated ? postcondPass : false;
    row["postcond_detail"] = postcondDetail;
    row["spawns"] = spawns;
    // F105: name the axis. golem counts GATED subprocess executions only —
    // a payload the gate refused never reaches this number, which is the
    // opposite convention from the ungated langgraph arm.
    row["spawns_definition"] = "gated subprocess executions (post-payload-gate)";
    row["gate_holds"] = gateHolds;
    row["solicit_fails"] = solicitFails;
    // F72 part 2: how many passes came from a RE-SOLICITED payload. A
    // round-2 pass is a different claim from a round-1 pass.
    row["resolicits"] = resolicits;
    row["wall_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    row["finished_at_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    results.push_back(row);
  }

  json out;
  out["harness"] = "golem";
  out["peak_ram_mb"] = peakRssMb();
  out["results"] = results;
  std::ofstream o(outPath);
  o << out.dump(1) << "\n";
  note("wrote " + outPath);
  return 0;
}
