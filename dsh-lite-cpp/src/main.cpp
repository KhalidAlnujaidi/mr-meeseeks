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

// ── --scope-dir: work on a real project without weakening isolation ──
//
// THE TENSION THIS RESOLVES: the sandbox gives every spawn a fresh
// ephemeral workspace and deletes it afterwards (correct — no host-side
// artifact trust), so `git status` returned "not a git repository" and no
// task could touch existing files. Copying the whole project in would be
// wrong twice over: the agent would edit a COPY and the host would never
// see the result, and the copy-back would be a bulk write into the user's
// project with no review.
//
// The design chosen, deliberately conservative:
//   1. INPUTS are STAGED: regular files under --scope-dir are copied into
//      the ephemeral workspace (depth-limited, size-capped, symlinks NOT
//      followed) so the agent sees a real, self-contained project.
//   2. The worker runs under the SAME sandbox as always — scrubbed env,
//      pinned CWD, watchdog, sanitized output. Isolation is unchanged.
//   3. OUTPUTS are NOT written back automatically. The workspace is kept
//      when --keep is given, and the summary lists what changed, so the
//      host reviews before anything touches the original. Auto-writing
//      model output into a real repo is exactly the unverified side effect
//      the gate exists to prevent.
struct ScopePlan {
  bool enabled = false;
  fs::path dir;
  std::size_t maxFiles = 200;
  std::size_t maxBytes = 8u * 1024 * 1024;  // total staged budget
  int maxDepth = 4;
};

// Copy regular files from `dir` into `dst` (mirroring relative paths).
// Symlinks are NOT followed: a link pointing outside the scope would
// otherwise pull host files into the workspace. Returns files staged.
std::size_t stageInputs(const fs::path& dir, const fs::path& dst,
                        const ScopePlan& plan, std::size_t& bytes) {
  std::size_t staged = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(
           dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (staged >= plan.maxFiles || bytes >= plan.maxBytes) break;
    const fs::path& p = it->path();
    // Depth guard: refuse to walk arbitrarily deep trees.
    const auto rel = fs::relative(p, dir, ec);
    if (ec) continue;
    const auto depth = std::distance(rel.begin(), rel.end());
    if (depth > plan.maxDepth) {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (it->is_symlink(ec)) continue;      // never follow links out of scope
    if (!it->is_regular_file(ec)) continue;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz > plan.maxBytes || bytes + sz > plan.maxBytes) continue;
    const fs::path target = dst / rel;
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(p, target, fs::copy_options::overwrite_existing, ec);
    if (ec) continue;
    bytes += sz;
    ++staged;
  }
  return staged;
}

// Files in the workspace that differ from (or are absent in) the scope
// dir — what the agent changed. Reported, never written back.
std::vector<std::string> changedFiles(const fs::path& ws, const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(ws, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const fs::path rel = fs::relative(it->path(), ws, ec);
    if (ec || rel.empty() || rel.native()[0] == '.') continue;
    const fs::path orig = dir / rel;
    if (!fs::exists(orig, ec)) { out.push_back(rel.string() + " (new)"); continue; }
    const auto a = fs::file_size(it->path(), ec);
    const auto b = fs::file_size(orig, ec);
    if (ec || a != b) { out.push_back(rel.string() + " (modified)"); continue; }
    std::ifstream fa(it->path(), std::ios::binary), fb(orig, std::ios::binary);
    std::string sa((std::istreambuf_iterator<char>(fa)), {});
    std::string sb((std::istreambuf_iterator<char>(fb)), {});
    if (sa != sb) out.push_back(rel.string() + " (modified)");
  }
  return out;
}

void printUsageHint() {
  std::cerr << "usage: golem [--offline] [--json] [--scope-dir DIR] [--keep]\n"
               "             [--max-files N] [\"task text\"]\n";
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
  // OUTPUT-side phrasing beats INPUT-side: "write the result to sorted.txt"
  // names what to CHECK. An earlier version grabbed the first filename in
  // the sentence (`numbers.txt`, the input) and verified the wrong artifact
  // — caught live on a scoped run where it reported "numbers.txt exists=true"
  // and called a sort task verified on its INPUT.
  if (d.file.empty()) d.file = findFile("result to");
  if (d.file.empty()) d.file = findFile("result into");
  if (d.file.empty()) d.file = findFile("output to");
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
  // --scope-dir reporting (empty when not scoping).
  std::size_t stagedFiles = 0;
  std::string keptWorkspace;
  std::vector<std::string> changed;
};

TurnReport runTurn(BrainLoop& brain, const std::string& task,
                   const ScopePlan& scope) {
  TurnReport out;

  // 0. --scope-dir: build the ephemeral workspace and stage the inputs
  // BEFORE the task runs, so the agent sees a real project. Also register
  // it as a postcondition-visible location for the F88-style copy-out.
  fs::path scopeWs;
  if (scope.enabled) {
    scopeWs = fs::temp_directory_path() / "golem" /
              ("scope_" + std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count()));
    std::error_code ec;
    fs::create_directories(scopeWs, ec);
    if (ec) {
      out.disposition = "scope-error";
      out.postcondDetail = "could not create staging workspace: " + ec.message();
      return out;
    }
    std::size_t bytes = 0;
    out.stagedFiles = stageInputs(scope.dir, scopeWs, scope, bytes);
    if (out.stagedFiles == 0) {
      out.disposition = "scope-error";
      out.postcondDetail =
          "no regular files staged from " + scope.dir.string() +
          " (check --max-files/--max-depth, and note symlinks are not followed)";
      return out;
    }
  }

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

  // --scope-dir: scope the staged project into the worker's own ephemeral
  // workspace. The spawner symlinks these in, so the agent edits COPIES;
  // isolation is unchanged and the originals are never touched.
  if (scope.enabled && !scopeWs.empty()) {
    std::error_code sec;
    for (auto it = fs::recursive_directory_iterator(scopeWs, sec);
         it != fs::recursive_directory_iterator(); it.increment(sec)) {
      if (sec) break;
      if (it->is_regular_file(sec)) opt.scopeFiles.push_back(it->path().string());
    }
  }

  // F72 layer 2 for the CLI: assert the outcome the task text actually
  // stated. Nothing declared => no hook => the runtime returns the honest
  // "verified-exit-only" label instead of a fake pass.
  //
  // --scope-dir rides in the same hook for the same reason the bench does
  // it (F88): this runs while the worker's ephemeral workspace still
  // EXISTS, so it is the only window in which the agent's edits can be
  // copied back to the staging dir for comparison. Nothing is written to
  // the user's --scope-dir here — only to our own staging copy.
  const DerivedCheck dc = deriveCheck(task);
  BrainLoop::PostconditionHook postcond =
      [gt = dc.groundTruth, &scopeWs](const std::string& ws) {
        PostconditionResult pc;
        if (!gt.is_null() && !gt.empty()) pc = checkPostcondition(ws, gt);
        if (!scopeWs.empty()) {
          std::error_code cec;
          for (auto it = fs::recursive_directory_iterator(ws, cec);
               it != fs::recursive_directory_iterator(); it.increment(cec)) {
            if (cec) break;
            if (!it->is_regular_file(cec)) continue;
            const fs::path rel = fs::relative(it->path(), ws, cec);
            if (cec || rel.empty() || rel.native()[0] == '.') continue;
            const fs::path dst = scopeWs / rel;
            fs::create_directories(dst.parent_path(), cec);
            fs::copy_file(it->path(), dst,
                          fs::copy_options::overwrite_existing, cec);
          }
        }
        return pc;
      };
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

  // --scope-dir: report what the agent changed, and keep or discard the
  // staging copy. NOTHING is written into the user's --scope-dir — that is
  // a deliberate review step, not an oversight.
  if (scope.enabled && !scopeWs.empty()) {
    out.changed = changedFiles(scopeWs, scope.dir);
    out.keptWorkspace = scopeWs.string();
  }
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
    if (r.stagedFiles || !r.keptWorkspace.empty()) {
      j["staged_files"] = r.stagedFiles;
      j["workspace"] = r.keptWorkspace;
      j["changed"] = r.changed;
    }
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
  if (r.stagedFiles || !r.keptWorkspace.empty()) {
    std::cout << "  scope      : " << r.stagedFiles << " file(s) staged\n";
    if (r.changed.empty())
      std::cout << "  changed    : (nothing — the project copy is unmodified)\n";
    else {
      std::cout << "  changed    : " << r.changed.size() << " file(s)\n";
      for (const auto& c : r.changed) std::cout << "               " << c << "\n";
    }
    if (!r.keptWorkspace.empty())
      std::cout << "  staged at  : " << r.keptWorkspace
                << "\n               (your --scope-dir was NOT written to)\n";
  }
  std::cout << "  => "
            << (r.ok ? "VERIFIED"
                     : "NOT VERIFIED (the artifact was not shown to be correct)")
            << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool offline = false, asJson = false, keep = false;
  std::string question;
  ScopePlan scope;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--offline") offline = true;
    else if (a == "--json") asJson = true;
    else if (a == "--keep") keep = true;
    else if (a == "-h" || a == "--help") { printUsageHint(); return 0; }
    else if (a == "--scope-dir" && i + 1 < argc) {
      scope.enabled = true;
      scope.dir = argv[++i];
    } else if (a.rfind("--scope-dir=", 0) == 0) {
      scope.enabled = true;
      scope.dir = a.substr(std::string("--scope-dir=").size());
    } else if (a == "--max-files" && i + 1 < argc) {
      scope.maxFiles = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (a == "--max-depth" && i + 1 < argc) {
      scope.maxDepth = std::stoi(argv[++i]);
    } else if (question.empty()) question = a;
    else {
      std::cerr << "golem: unexpected argument '" << a << "'\n";
      printUsageHint();
      return 2;
    }
  }
  if (offline) return offlineDemo();

  // Validate the scope dir BEFORE any engine work: a typo must fail fast
  // and loudly, not silently run without the project staged.
  if (scope.enabled) {
    std::error_code ec;
    if (!fs::is_directory(scope.dir, ec)) {
      std::cerr << "golem: --scope-dir is not a readable directory: "
                << scope.dir << "\n";
      return 2;
    }
    scope.dir = fs::absolute(scope.dir, ec);
  }

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
    const TurnReport r = runTurn(brain, question, scope);
    printReport(r, asJson);
    if (!keep && !r.keptWorkspace.empty()) {
      std::error_code ec;
      fs::remove_all(r.keptWorkspace, ec);  // discard the staging copy
    }
    return r.ok ? 0 : 1;
  }

  // Interactive REPL. Ctrl-D exits.
  std::cout << "Golem — host-enforced agent loop. Engine: " << cfg.endpoint
            << " (" << cfg.model << ")\n"
            << "One task per line. 'exit' to quit. Every command is gated,\n"
            << "sandboxed and verified; nothing runs on the model's say-so.\n";
  if (scope.enabled)
    std::cout << "Scoped to: " << scope.dir
              << " (inputs staged per turn; your directory is never written)\n";
  std::string line;
  while (true) {
    std::cout << "\ngolem> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (line == "exit" || line == "quit") break;
    if (line.empty()) continue;
    const TurnReport r = runTurn(brain, line, scope);
    printReport(r, asJson);
    if (!keep && !r.keptWorkspace.empty()) {
      std::error_code ec;
      fs::remove_all(r.keptWorkspace, ec);
    }
  }
  std::cout << "bye\n";
  return 0;
}
