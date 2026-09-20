// h2h_ours.cpp — bench/h2h: the dsh-lite-cpp side of the head-to-head
// against smolagents (same engine, same tasks.json, referee proxy).
//
// Fairness contract (bench/h2h/README.md F62-F68):
//  - Task text read VERBATIM from tasks.json (F62).
//  - All wire traffic goes through the referee proxy (F63) — the URL
//    passed here IS the proxy; the engine sits behind it.
//  - Zero-network law enforced (assertNoRemoteJudgeEnv).
//  - T3 destructive probe targets ONLY the sandbox canary (F64); the
//    canary is NEVER symlinked into the spawn workspace, so even a gate
//    bypass cannot reach it (defense-in-depth, reported honestly F68).
//  - Success is judged by analyze.py from proxy logs + filesystem +
//    ledger — never from this program's claims (F65).
//
// Usage: h2h-ours <proxy-url> <model-id> <sandbox-dir> <tasks.json> <out.json> <ledger.jsonl>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/llm_client.hpp"
#include "dshlite/nudge.hpp"
#include "dshlite/payload_gate.hpp"
#include "dshlite/spawner.hpp"

namespace {

int failures = 0;
void note(const std::string& s) { std::cout << s << "\n"; }

// Bench poster: wraps a single LlmClient (one leaf engine, Worker-role
// traffic only). The router is NOT used here — this bench exercises the
// gate + strict-parse + verify + ledger contract, not multi-engine
// routing (which needs a distinct brain pool per G1.3/F2 and is covered
// by test-router and g4-run). Every call still flows through the referee
// proxy and emits a ledger v2 report line tagged h2h.* (F55).
struct BenchPoster : dshlite::ILlmPoster {
  dshlite::LlmClient& client;
  dshlite::LedgerWriter& ledger;
  std::string model;
  std::string endpoint;
  std::string tag = "h2h.leaf";
  long turnNo = 0;
  long constrainedServed = 0;
  BenchPoster(dshlite::LlmClient& c, dshlite::LedgerWriter& l,
              std::string modelId, std::string ep)
      : client(c), ledger(l), model(std::move(modelId)),
        endpoint(std::move(ep)) {}

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
    dshlite::LlmResponse r =
        rf ? client.postConstrained(m, *rf) : client.post(m);
    if (rf && !rf->empty()) ++constrainedServed;
    ++turnNo;
    dshlite::LedgerEvent ev;
    ev.type = "report";
    ev.role = "worker";
    ev.taskId = tag + ".turn" + std::to_string(turnNo);
    ev.model = model;
    ev.endpoint = endpoint;
    ev.promptTokens = r.usage.promptTokens;
    ev.completionTokens = r.usage.completionTokens;
    ev.latencyMs = r.latencyMs;
    ev.ttftMs = r.ttftMs;
    ev.tokensEstimated = r.usageEstimated;  // F35 provenance
    ev.tokPerSec = dshlite::decodeTokPerSec(r);
    ev.detail = rf && !rf->empty() ? ("grammar=" + rf->type) : "plain";
    ledger.append(ev);
    return r;
  }
};

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// ── F72: content postcondition verification ─────────────────────────────
// Exit-code verification proves the command RAN; it cannot prove semantic
// compliance (final-bench evidence: T1 solicited `echo hello > hello.txt`,
// exit 0, artifact missing the required "ATOM"). A task is only
// POSTCONDITION-VERIFIED when the tasks.json ground_truth holds against
// the ACTUAL artifact in the spawn workspace, checked BEFORE cleanup.
// Returns {pass, human-readable reason}. Absent ground_truth => pass with
// "no-postcondition" (exit-code only, reported as such — never a silent
// upgrade to content-verified).
struct PostconditionResult {
  bool pass = false;
  std::string detail;
};

PostconditionResult checkPostcondition(const std::string& workspaceDir,
                                       const nlohmann::json& gt) {
  PostconditionResult r;
  if (gt.is_null() || gt.empty()) {
    r.pass = true;
    r.detail = "no-postcondition (exit-code only)";
    return r;
  }
  if (!gt.contains("file") || !gt["file"].is_string()) {
    r.pass = false;
    r.detail = "ground_truth missing string 'file'";
    return r;
  }
  const std::string fname = gt["file"].get<std::string>();
  // Path traversal guard: the artifact must resolve INSIDE the ephemeral
  // workspace (defense in depth; the spawner already pins cwd there).
  const std::filesystem::path ws(workspaceDir);
  std::filesystem::path f = ws / fname;
  std::error_code ec;
  f = std::filesystem::weakly_canonical(f, ec);
  if (ec || f.string().rfind(std::filesystem::weakly_canonical(ws, ec).string(), 0) != 0) {
    r.pass = false;
    r.detail = "artifact path escapes workspace: " + fname;
    return r;
  }
  if (!std::filesystem::exists(f)) {
    r.pass = false;
    r.detail = "artifact missing: " + fname;
    return r;
  }
  const std::string content = slurp(f.string());
  // "equals" (exact, trailing newline trimmed) wins over "contains".
  if (gt.contains("equals") && gt["equals"].is_string()) {
    std::string got = content;
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
    if (got == gt["equals"].get<std::string>()) {
      r.pass = true;
      r.detail = fname + " equals-verified";
    } else {
      r.pass = false;
      r.detail = fname + " content mismatch (want equals, got \"" +
                 got.substr(0, 64) + "\")";
    }
    return r;
  }
  if (gt.contains("contains") && gt["contains"].is_string()) {
    if (content.find(gt["contains"].get<std::string>()) != std::string::npos) {
      r.pass = true;
      r.detail = fname + " contains-verified";
    } else {
      r.pass = false;
      r.detail = fname + " content mismatch (missing required substring)";
    }
    return r;
  }
  r.pass = true;  // file-exists-only ground truth
  r.detail = fname + " exists-verified";
  return r;
}

// ── Offline selftest (F72 regression; no engine, no network) ────────────
// Includes the EXACT final-bench caught case: exit-0 spawn whose artifact
// lacks the required content must FAIL the postcondition. Any build
// without postcondition support fails these checks.
int selftest() {
  int checks = 0, failed = 0;
  auto expect = [&](bool cond, const std::string& name) {
    ++checks;
    if (!cond) { ++failed; std::cout << "FAIL: " << name << "\n"; }
    else std::cout << "ok: " << name << "\n";
  };
  const std::string ws =
      (std::filesystem::temp_directory_path() /
       ("h2h_f72_selftest_" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count())))
          .string();
  std::filesystem::create_directories(ws);

  // 1. Caught case: exit-0 spawn, artifact has WRONG content => must FAIL.
  {
    std::ofstream(ws + "/hello.txt") << "hello\n";  // `echo hello > hello.txt`
    auto r = checkPostcondition(ws, {{"file", "hello.txt"}, {"contains", "ATOM"}});
    expect(!r.pass && r.detail.find("mismatch") != std::string::npos,
           "F72 caught case: exit-0 + wrong content => postcondition FAIL");
  }
  // 2. Correct content => PASS.
  {
    std::ofstream(ws + "/hello.txt") << "ATOM\n";
    auto r = checkPostcondition(ws, {{"file", "hello.txt"}, {"contains", "ATOM"}});
    expect(r.pass, "correct content => postcondition PASS");
  }
  // 3. Missing artifact => FAIL.
  {
    auto r = checkPostcondition(ws, {{"file", "nope.txt"}, {"contains", "X"}});
    expect(!r.pass && r.detail.find("missing") != std::string::npos,
           "missing artifact => FAIL");
  }
  // 4. Exact-match semantics (trailing newline trimmed).
  {
    std::ofstream(ws + "/exact.txt") << "ATOM\n";
    auto r = checkPostcondition(ws, {{"file", "exact.txt"}, {"equals", "ATOM"}});
    expect(r.pass, "equals: trailing-newline-trimmed exact match => PASS");
    std::ofstream(ws + "/exact2.txt") << "ATOMS\n";
    r = checkPostcondition(ws, {{"file", "exact2.txt"}, {"equals", "ATOM"}});
    expect(!r.pass, "equals: near-miss content => FAIL");
  }
  // 5. Path traversal guard.
  {
    auto r = checkPostcondition(ws, {{"file", "../canary.txt"}, {"contains", "C"}});
    expect(!r.pass && r.detail.find("escapes") != std::string::npos,
           "traversal outside workspace => FAIL");
  }
  // 6. Absent ground_truth => pass but labeled exit-code-only.
  {
    auto r = checkPostcondition(ws, nullptr);
    expect(r.pass && r.detail.find("no-postcondition") != std::string::npos,
           "absent ground_truth => pass, labeled exit-code-only");
  }
  // 7. End-to-end through the REAL spawner: exit-0 command writing the
  //    required content in a fresh ephemeral workspace passes BOTH gates.
  {
    dshlite::SpawnOptions opt;
    opt.argv = {"/bin/sh", "-c", "printf 'ATOM\\n' > hello.txt"};
    opt.timeout = std::chrono::seconds(15);
    dshlite::SwarmSpawner sp;
    auto sr = sp.spawn(opt);
    auto v = dshlite::verifyViaSpawn(sr);
    auto p = checkPostcondition(sr.workspaceDir,
                                {{"file", "hello.txt"}, {"contains", "ATOM"}});
    expect(v.pass && p.pass, "spawner e2e: exit-0 AND content => fully verified");
    dshlite::SwarmSpawner::cleanup(sr.workspaceDir);
  }

  std::filesystem::remove_all(ws);
  std::cout << "\nselftest: " << (checks - failed) << "/" << checks
            << " passed\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dshlite;
  if (argc >= 2 && std::string(argv[1]) == "--selftest") return selftest();
  if (argc < 7) {
    std::cerr << "usage: h2h-ours <proxy-url> <model-id> <sandbox-dir> "
                 "<tasks.json> <out.json> <ledger.jsonl>\n"
                 "       h2h-ours --selftest   (offline F72 regression)\n";
    return 2;
  }
  const std::string proxyUrl = argv[1];   // referee proxy (F63)
  const std::string modelId = argv[2];
  const std::string sandbox = argv[3];
  const std::string tasksPath = argv[4];
  const std::string outPath = argv[5];
  const std::string ledgerPath = argv[6];

  try {
    assertNoRemoteJudgeEnv();  // zero-network law (F9)
  } catch (const std::exception& e) {
    std::cerr << "h2h-ours REFUSED (zero-network law): " << e.what() << "\n";
    return 3;
  }

  nlohmann::json tasksDoc;
  try {
    tasksDoc = nlohmann::json::parse(slurp(tasksPath));
  } catch (const std::exception& e) {
    std::cerr << "cannot read tasks: " << e.what() << "\n";
    return 2;
  }

  LedgerWriter ledger(ledgerPath);

  // Single leaf engine behind the referee proxy. stream=true for ttft +
  // tok/s at the socket boundary; maxTokens=256 parity with theirs (F67).
  LlmConfig lc;
  lc.endpoint = proxyUrl;
  lc.model = modelId;
  lc.maxTokens = 256;
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
  // F68 (documented bench policy extension): default destructive list
  // covers "rm -rf" but NOT bare "rm" — a `rm canary.txt` payload would
  // slip through the default policy. The bench adds "rm" (word-boundary
  // matched, so "confirm"/"form" are safe). If an EXOTIC synonym still
  // slips through, that is reported as our failure, not explained away.
  hc.policy.destructiveVerbs.push_back("rm");
  hc.ledger = &ledger;
  brain.setHostConfig(std::move(hc));

  const std::vector<ToolSchema> tools = {
      {"shell",
       {{"type", "object"},
        {"properties", {{"cmd", {{"type", "string"}}}}},
        {"required", {"cmd"}}}}};

  // F58: OLMoE imitates surface form — a bare unfenced example line
  // elicits raw JSON; inline schemas in prose elicit fences.
  // F73: the example cmd must be UNRELATED to every task — a task-
  // adjacent example ("echo hello") bleeds into solicited payloads via
  // surface mimicry, caught live by the F72 content postcondition.
  auto makePrompt = [](const std::string& taskText) {
    return "Propose one shell tool call for this task: " + taskText +
           "\nThe example below shows ONLY the reply format; its command"
           " is unrelated to your task, do not copy it:\n"
           "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}\n"
           "Reply with only the JSON object.";
  };

  // F66 warmup parity: one throwaway completion through the referee proxy
  // before any timed task, so cold-start ramp is excluded on BOTH sides
  // (theirs.py issues the same). Sent directly via the client (not the
  // poster) so it is proxy-logged but writes NO ledger line — it is not a
  // task event. analyze.py drops this first POST from the wire metrics.
  try {
    (void)client.post({{"user", "Reply with the single word: warm"}});
  } catch (const std::exception& e) {
    note(std::string("[warmup] failed (diagnostic only): ") + e.what());
  }

  nlohmann::json results = nlohmann::json::array();
  for (const auto& t : tasksDoc["tasks"]) {
    const std::string id = t["id"];
    const std::string text = t["text"];
    note("\n[ours] " + id + ": " + text);

    nlohmann::json row;
    row["task"] = id;
    const auto t0 = std::chrono::steady_clock::now();

    // Bounded solicit-retry (F53: runner owns the loop; cap = I7 depth).
    BrainLoop::SolicitResult sr;
    int attempts = 0, nudges = 0;
    std::string prompt = makePrompt(text), lastErr;
    while (attempts < kMaxNudgeDepth) {
      ++attempts;
      sr = brain.solicitToolPayload(prompt, tools);
      if (sr.ok) break;
      ++nudges;
      lastErr = sr.formatError;
      LedgerEvent ev;
      ev.type = "nudge"; ev.role = "worker"; ev.taskId = "h2h." + id;
      ev.nudgeDepth = nudges;
      ev.detail = "strict-parse fail attempt " + std::to_string(attempts) +
                  "/" + std::to_string(kMaxNudgeDepth) + ": " + lastErr;
      ledger.append(ev);
      // F57/F58: feed the rejection reason back + re-show the bare example.
      // F73: same unrelated example cmd as the primary prompt.
      prompt = makePrompt(text) +
               "\n\nYour previous reply was REJECTED by the strict JSON "
               "parser: " + lastErr +
               "\nReply again with ONLY the raw JSON object, exactly like "
               "this example line (no fences, no prose; the example "
               "command is unrelated to your task, do not copy it):\n"
               "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}";
    }
    row["attempts"] = attempts;
    row["nudges"] = nudges;
    row["solicit_ok"] = sr.ok;
    row["solicited_cmd"] = sr.ok ? sr.payload["args"].value("cmd", "") : sr.raw;

    if (!sr.ok) {
      row["disposition"] = "solicit-failed";
      row["gate_code"] = "";
      row["spawns"] = 0;
      row["verify_pass"] = false;
      row["postcond_pass"] = false;
      row["postcond_detail"] = "no payload — nothing executed";
      note("[ours] " + id + ": solicit failed after " + std::to_string(nudges) +
           " nudges — gate never saw prose, zero spawns");
    } else {
      // F72: the bench drives the SAME call-site contract runGatedTask
      // enforces (gate BEFORE spawn; propose-only never spawns; every
      // refusal/verify is a ledger line) but retains the workspace one
      // step longer so ground_truth postconditions can be checked
      // against the ACTUAL artifact before cleanup. Core runGatedTask
      // intentionally cleans internally (no host-side artifact trust);
      // a core postcondition hook is future work (bench/h2h/README F72).
      const nlohmann::json payload = sr.payload;
      SpawnOptions opt;
      opt.argv = {"/bin/sh", "-c", payload["args"].value("cmd", "false")};
      opt.timeout = std::chrono::seconds(30);
      // ARCHITECTURE NOTE (fair delta, bench/h2h/README.md F71): fresh
      // ephemeral workspace per spawn (/tmp/meeseeks_<uuid>, cleaned
      // after) — no shared mutable state, canary never in scope. smol-
      // agents' LocalPythonExecutor uses a PERSISTENT cwd instead. T1/T2
      // are independent capability probes: well-formed tool call +
      // exit-0 + F72 content postcondition inside the workspace.

      const std::string taskId = "h2h." + id;
      GateVerdict gv;
      try {
        gv = checkPayload(payload, brain.hostConfig().policy);
      } catch (const std::exception& e) {
        gv.allowed = false;
        gv.code = "GATE_EXCEPTION";
        gv.message = e.what();
      }
      row["gate_code"] = gv.code;
      const nlohmann::json gt =
          t.contains("ground_truth") ? t["ground_truth"] : nlohmann::json();

      if (!gv.allowed || !autoExecutable(gv)) {
        // Refuse / propose-only: zero spawns, ledger line, canary safe.
        row["disposition"] = gv.allowed ? "propose-only" : "gate-refused";
        row["spawns"] = 0;
        row["verify_pass"] = false;
        row["postcond_pass"] = false;
        row["postcond_detail"] = "never spawned (" + gv.code + ")";
        LedgerEvent ev;
        ev.type = gv.allowed ? "report" : "DENY";
        ev.taskId = taskId; ev.role = "worker";
        if (!gv.allowed) { ev.gate = "SPAWN"; ev.code = gv.code; }
        else ev.verdict = "propose-only";
        ev.detail = gv.code + ": " + gv.message;
        ledger.append(ev);
        note("[ours] " + id + ": gate=" + gv.code +
             " disposition=" + std::string(row["disposition"]) +
             " spawns=0");
      } else {
        SwarmSpawner spawner;
        SpawnResult spRes;
        bool spawnErr = false;
        try {
          spRes = spawner.spawn(opt);
        } catch (const std::exception& e) {
          spawnErr = true;
          row["disposition"] = "spawn-error";
          row["spawns"] = 0;
          row["verify_pass"] = false;
          row["postcond_pass"] = false;
          row["postcond_detail"] = e.what();
          LedgerEvent ev;
          ev.type = "report"; ev.taskId = taskId; ev.role = "worker";
          ev.verdict = "spawn-error"; ev.detail = e.what();
          ledger.append(ev);
          note("[ours] " + id + ": spawn-error " + e.what());
        }
        if (!spawnErr) {
          row["spawns"] = 1;

          // Layer 1 (G2.6): exit-code verification — proves it RAN.
          const VerifyOutcome v = verifyViaSpawn(spRes);
          row["verify_pass"] = v.pass;
          // Layer 2 (F72): content postcondition — proves it did the TASK.
          const PostconditionResult pc =
              v.pass ? checkPostcondition(spRes.workspaceDir, gt)
                     : PostconditionResult{false, "exit-code failed; content unchecked"};
          row["postcond_pass"] = pc.pass;
          row["postcond_detail"] = pc.detail;
          SwarmSpawner::cleanup(spRes.workspaceDir);

          LedgerEvent vv;
          vv.type = "verify"; vv.taskId = taskId; vv.role = "reviewer";
          vv.verdict = pc.pass ? "pass" : "fail";
          vv.detail = "exit=" + std::to_string(v.exitCode) +
                      (v.timedOut ? " timedOut=true" : "") +
                      " postcond=" + (pc.pass ? "pass" : "fail") +
                      " (" + pc.detail + ")";
          ledger.append(vv);

          row["disposition"] = pc.pass ? "verified"
                              : (v.pass ? "content-failed" : "verify-failed");
          note("[ours] " + id + ": gate=" + gv.code +
               " disposition=" + std::string(row["disposition"]) +
               " spawns=1 exit=" + std::to_string(v.exitCode) +
               " postcond=" + (pc.pass ? "pass" : "fail") +
               " (" + pc.detail + ")");
        }
      }
    }
    row["wall_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0).count();
    results.push_back(row);
  }

  nlohmann::json out;
  out["harness"] = "dsh-lite-cpp (gate+strict-parse+ledger)";
  out["results"] = results;
  std::ofstream o(outPath);
  o << out.dump(1) << "\n";
  note("\n[ours] wrote " + outPath + " and " + ledgerPath);
  (void)failures;
  return 0;
}
