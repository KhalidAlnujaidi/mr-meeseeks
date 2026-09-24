// main.cpp — Golem CLI agent loop.
//
// This is the AGENT entry point: an interactive loop that drives the full
// host-enforced stack end to end —
//
//   solicit (strict parse)  ->  pre-execution payload gate  ->  sandboxed
//   spawn  ->  dual-layer verify (exit code AND content postcondition)
//   ->  bounded retry with host-driven re-solicitation
//
// Everything that makes Golem Golem runs here, in-process: the Brain keeps
// strategy, the host owns the gate and the cap ladder, and the model never
// votes itself out of either. The model proposes; the host disposes.
//
// Usage:
//   golem                              # interactive REPL against the engine
//   golem "create a file note.txt containing ISO-ATOM"
//   golem --offline                    # no engine: stack self-demo
//   golem --json "..."                 # emit one machine-readable report
//
// Engine (another shell): coli serve --model <weights> --model-id olmoe-leaf
// Loopback needs no key; COLI_API_KEY only for a remote bind.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
#include "dshlite/sanitizer.hpp"
#include "dshlite/spawner.hpp"

namespace fs = std::filesystem;
using namespace dshlite;
using json = nlohmann::json;

namespace {

struct EchoLlm : ILlmPoster {
  LlmResponse post(const std::vector<Message>& m) override {
    return LlmResponse{"(offline) strategy noted for: " +
                           (m.empty() ? std::string{} : m.back().content),
                       {}, 0};
  }
};

// The ONE tool this agent exposes. Kept deliberately single-tool: the
// payload grammar's tool-name enum is trivially satisfiable, and F100
// records that the model still invents command names on this engine.
const std::vector<ToolSchema> kTools = {
    {"shell",
     {{"type", "object"},
      {"properties", {{"cmd", {{"type", "string"}}}}},
      {"required", {"cmd"}}}}};

// F91/F45 framing: the example is task-unrelated and marked do-not-copy
// (F73 prompt-bleed — an earlier bare `echo hello` example was copied
// verbatim into the solicited command and broke T2).
std::string makePrompt(const std::string& task) {
  return "Propose ONE shell tool call for this task: " + task +
         "\nThe example below shows ONLY the reply format; its command is"
         " unrelated to your task, do not copy it:\n"
         "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}\n"
         "Reply with only the JSON object.";
}

// payload -> argv, the single definition used for the initial payload and
// every re-solicited one (F99: without this a fresh payload is gated and
// then ignored, because the spawn would still use the original argv).
void bindExec(SpawnOptions& so, const json& p) {
  so.argv = {"/bin/sh", "-c", p["args"].value("cmd", "false")};
}

void printUsageHint() {
  std::cerr << "usage: golem [--offline] [--json] [\"task text\"]\n";
}

// Offline self-demo: proves the stack without an engine (and is what the
// original main.cpp did, preserved so nothing regresses).
int offlineDemo() {
  SwarmSpawner sp;
  SpawnOptions o;
  o.argv = {"printf", "\033[32mworker done\033[0m \001\n"};
  SpawnResult r = sp.spawn(o);
  std::cout << "worker exit=" << r.exitCode << " brain-sees=["
            << r.sanitizedStdout << "]\n";
  SwarmSpawner::cleanup(r.workspaceDir);

  EchoLlm llm;
  BrainLoop brain(llm, [](const std::string& c) -> JudgeVerdict {
    JudgeVerdict v;
    v.route = "do_direct";
    v.confidence = 0.9;
    v.action = JudgeAction::DoDirect;
    v.detail = "stub judge: atomic (" + c + ")";
    return v;
  });
  brain.setSystemPrompt("You are the executive Brain. Never execute labor.");
  std::cout << brain.turn("draft the milestone plan") << "\n";
  JudgeVerdict g = brain.gateDelegation("write the changelog");
  std::cout << "gate: action=" << static_cast<int>(g.action)
            << " route=" << g.route << " conf=" << g.confidence << "\n";
  std::cout << "OFFLINE DEMO PASS\n";
  return 0;
}

// Extract a machine-checkable postcondition from a plain-English task.
//
// WHY THIS EXISTS (honesty note): `runGatedTask` gives three honest
// labels — "verified" (exit0 AND a declared postcondition held),
// "verified-exit-only" (exit0, nothing declared), and the failure states.
// A CLI has no sealed ground truth to assert, so without this it would
// either report every run as NOT VERIFIED (useless) or call exit-0 a pass
// (exactly the F72 lie the runtime exists to prevent). So the CLI derives
// a postcondition ONLY from patterns the task text states explicitly
// (`exactly the text X`, `containing X`, `named <file>`), and otherwise
// declares nothing and stays honestly labeled.
//
// This is a heuristic, and it is deliberately narrow: an unparsed task
// falls through to verified-exit-only rather than a guessed assertion.
struct DerivedCheck {
  bool haveFile = false;
  std::string file;
  json groundTruth;  // empty json when nothing was asserted
};

DerivedCheck deriveCheck(const std::string& task) {
  DerivedCheck d;
  auto findAfter = [&](const std::string& key) -> std::string {
    const auto p = task.find(key);
    if (p == std::string::npos) return {};
    std::string rest = task.substr(p + key.size());
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    // strip a leading quote, then stop at a quote or sentence end
    if (!rest.empty() && (rest.front() == '\'' || rest.front() == '"'))
      rest.erase(rest.begin());
    std::string val;
    for (const char c : rest) {
      if (c == '\'' || c == '"' || c == '.' || c == '\n') break;
      val.push_back(c);
    }
    while (!val.empty() && val.back() == ' ') val.pop_back();
    return val;
  };
  auto findFile = [&](const std::string& key) -> std::string {
    const auto p = task.find(key);
    if (p == std::string::npos) return {};
    std::string rest = task.substr(p + key.size());
    while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
    std::string val;
    // NOTE: '.' is NOT a terminator here — filenames contain dots. An
    // earlier version stopped at '.', parsed "note.txt" as "note", and the
    // postcondition then failed against a file that existed (caught live
    // by the dual-layer check, which is the point of having it).
    for (const char c : rest) {
      if (c == ' ' || c == '"' || c == '\'' || c == '\n' || c == ',')
        break;
      val.push_back(c);
    }
    return val;
  };

  d.file = findFile("file named");
  if (d.file.empty()) d.file = findFile("named");
  if (d.file.empty()) {
    // fall back to the first token that looks like a filename (has a dot)
    std::stringstream ss(task);
    std::string w;
    while (ss >> w) {
      std::string clean;
      for (const char c : w)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
            c == '_' || c == '-' || c == '/')
          clean.push_back(c);
      if (clean.find('.') != std::string::npos && clean.size() > 2 &&
          clean[0] != '.') {
        d.file = clean;
        break;
      }
    }
  }
  d.haveFile = !d.file.empty();

  const std::string exact = findAfter("exactly the text");
  const std::string contains = findAfter("containing");
  if (d.haveFile && !exact.empty()) {
    d.groundTruth = {{"file", d.file}, {"equals", exact}};
  } else if (d.haveFile && !contains.empty()) {
    d.groundTruth = {{"file", d.file}, {"contains", contains}};
  } else if (d.haveFile) {
    // File named, content unstated: assert existence only. Still a real
    // assertion (the file must exist) and still honest.
    d.groundTruth = {{"file", d.file}, {"exists", true}};
  }
  return d;
}

// One agent turn: solicit -> gate -> spawn -> verify, with bounded
// re-solicitation. Returns a report the caller can print or serialize.
struct TurnReport {
  bool ok = false;
  std::string disposition;
  std::string cmd;
  std::string postcondDetail;
  int exitCode = -1;
  int spawns = 0;
  int resolicits = 0;
  std::string gateCode;
  std::string gateMessage;
  json checked;  // the postcondition actually asserted (empty if none)
};

TurnReport runTurn(BrainLoop& brain, const std::string& task) {
  TurnReport out;

  // 1. Solicit, with bounded parse retries (F53 cap = nudge depth law).
  std::string prompt = makePrompt(task);
  BrainLoop::SolicitResult sr;
  for (int attempt = 0; attempt < kMaxNudgeDepth; ++attempt) {
    sr = brain.solicitToolPayload(prompt, kTools);
    if (sr.ok) break;
    prompt = makePrompt(task) +
             "\n\nYour previous reply was REJECTED by the strict JSON parser."
             " Reply again with ONLY the raw JSON object, exactly like the"
             " example line (no fences, no prose):\n"
             "{\"tool\":\"shell\",\"args\":{\"cmd\":\"date > stamp.txt\"}}";
  }
  if (!sr.ok) {
    out.disposition = "solicit-failed";
    out.postcondDetail = sr.formatError.empty()
                             ? "strict parse failed"
                             : ("strict parse failed: " + sr.formatError);
    return out;
  }

  // 2. The host owns gate + verify + caps: hand the payload to runGatedTask
  // with the re-solicit hook and the execution binder. Nothing here lets
  // the model skip a layer.
  int hookCalls = 0;
  constexpr int kMaxResolicits = 1;
  auto resolicit = [&](const BrainLoop::FailureFeedback& fb)
      -> std::optional<json> {
    ++hookCalls;
    std::string rp = makePrompt(task);
    rp += "\n\nYour previous command did not produce the required result.";
    if (!fb.detail.empty()) rp += " Problem: " + fb.detail;
    BrainLoop::SolicitResult ns = brain.solicitToolPayload(rp, kTools);
    if (!ns.ok) return std::nullopt;  // no fresh payload => replay
    return ns.payload;
  };
  BrainLoop::RetryPlanner planner = [&](int, json&) -> RetryScope {
    return hookCalls < kMaxResolicits ? RetryScope::NarrowOrReroute
                                      : RetryScope::FullScope;
  };

  SpawnOptions opt;
  opt.timeout = std::chrono::seconds(60);
  bindExec(opt, sr.payload);

  // F72 layer 2 for the CLI: assert the outcome the task text actually
  // stated. Nothing declared => no hook => the runtime returns the honest
  // "verified-exit-only" label instead of a fake pass.
  const DerivedCheck dc = deriveCheck(task);
  BrainLoop::PostconditionHook postcond;
  if (!dc.groundTruth.is_null() && !dc.groundTruth.empty()) {
    postcond = [gt = dc.groundTruth](const std::string& ws) {
      return checkPostcondition(ws, gt);
    };
  }

  const auto rep = brain.runGatedTask("cli." + std::to_string(
                                                   std::chrono::steady_clock::now()
                                                       .time_since_epoch()
                                                       .count()),
                                     sr.payload, opt, planner, postcond,
                                     resolicit, kMaxResolicits, bindExec);

  out.disposition = rep.disposition;
  out.cmd = sr.payload["args"].value("cmd", "");
  out.exitCode = rep.verify.exitCode;
  out.spawns = rep.spawns;
  out.resolicits = rep.resolicits;
  out.gateCode = rep.gate.code;
  out.gateMessage = rep.gate.message;
  out.postcondDetail = rep.postcondition.evaluated
                           ? rep.postcondition.detail
                           : rep.verify.detail;
  out.checked = rep.postcondition.evaluated ? dc.groundTruth : json();
  // "verified" requires BOTH layers (F72). "verified-exit-only" means the
  // command ran but nothing asserted its content — reported as such, never
  // upgraded to a pass.
  out.ok = (rep.disposition == "verified");
  if (rep.disposition == "verified-exit-only" && out.postcondDetail.empty())
    out.postcondDetail = "exit 0; task stated no checkable outcome";
  return out;
}

void printReport(const TurnReport& r, bool asJson) {
  if (asJson) {
    json j{{"disposition", r.disposition},
           {"verified", r.ok},
           {"command", r.cmd},
           {"exit_code", r.exitCode},
           {"spawns", r.spawns},
           {"resolicits", r.resolicits},
           {"gate_code", r.gateCode},
           {"checked", r.checked},
           {"detail", r.postcondDetail}};
    std::cout << j.dump() << "\n";
    return;
  }
  std::cout << "  command    : " << (r.cmd.empty() ? "(none)" : r.cmd) << "\n";
  std::cout << "  disposition: " << r.disposition << "\n";
  if (!r.gateCode.empty() && r.gateCode != "OK")
    std::cout << "  gate       : " << r.gateCode << " — " << r.gateMessage
              << "\n";
  std::cout << "  verify     : " << r.postcondDetail
            << (r.exitCode >= 0 ? (" (exit " + std::to_string(r.exitCode) + ")")
                                : "")
            << "\n";
  std::cout << "  labor      : " << r.spawns << " spawn(s), " << r.resolicits
            << " re-solicit(s)\n";
  std::cout << "  => "
            << (r.ok ? "VERIFIED"
                     : "NOT VERIFIED (the artifact was not shown to be correct)")
            << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool offline = false, asJson = false;
  std::string question;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--offline") offline = true;
    else if (a == "--json") asJson = true;
    else if (a == "-h" || a == "--help") { printUsageHint(); return 0; }
    else if (question.empty()) question = a;
  }
  if (offline) return offlineDemo();

  try {
    assertNoRemoteJudgeEnv();  // zero-network law (F9)
  } catch (const std::exception& e) {
    std::cerr << "golem REFUSED (zero-network law): " << e.what() << "\n";
    return 3;
  }

  // Ledger: append-only audit trail beside the other runtime artifacts.
  const fs::path stateDir = fs::temp_directory_path() / "golem";
  fs::create_directories(stateDir);
  const fs::path ledgerPath = stateDir / "cli-ledger.jsonl";
  LedgerWriter ledger(ledgerPath.string());

  LlmConfig cfg;  // defaults: 127.0.0.1:8000 + engine model-id
  if (const char* m = std::getenv("COLI_MODEL_ID")) cfg.model = m;
  if (const char* e = std::getenv("COLI_ENDPOINT")) cfg.endpoint = e;
  cfg.maxTokens = 256;  // parity with the bench protocol
  LlmClient client(cfg);
  BrainLoop brain(client, makeNodeJudgeHook());
  BrainLoop::HostConfig hc;
  hc.policy.allowedTools = {"shell"};
  // F68 (policy coverage is the operator's job): the stock destructive list
  // covers "rm -rf" but NOT a bare `rm`, so `rm canary.txt` would have been
  // EXECUTED — caught live while testing this CLI (3 spawns on a task that
  // should have been propose-only, exit 1). A CLI is exactly where the
  // operator is, so the coverage gap is closed here rather than inherited.
  hc.policy.destructiveVerbs.push_back("rm");
  hc.policy.destructiveVerbs.push_back("unlink");
  hc.policy.destructiveVerbs.push_back("shred");
  hc.ledger = &ledger;
  brain.setHostConfig(std::move(hc));

  // Non-interactive: one task, one line, exit code reflects verification.
  if (!question.empty()) {
    const TurnReport r = runTurn(brain, question);
    printReport(r, asJson);
    return r.ok ? 0 : 1;
  }

  // Interactive REPL. Ctrl-D exits.
  std::cout << "Golem — host-enforced agent loop. Engine: " << cfg.endpoint
            << " (" << cfg.model << ")\n"
            << "One task per line. 'exit' to quit. Every command is gated,\n"
            << "sandboxed and verified; nothing runs on the model's say-so.\n";
  std::string line;
  while (true) {
    std::cout << "\ngolem> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (line == "exit" || line == "quit") break;
    if (line.empty()) continue;
    const TurnReport r = runTurn(brain, line);
    printReport(r, asJson);
  }
  std::cout << "bye\n";
  return 0;
}
