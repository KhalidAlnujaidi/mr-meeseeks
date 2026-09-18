// harness_c.cpp — C ABI over the C++ host bundle (JSON in/out, noexcept).
//
// Minimal dependency-free JSON field extraction (string/int/bool/words) so
// the ABI layer adds no third-party surface. Malformed JSON => defaults,
// never exceptions across the boundary.

#include "harness_c.h"

#include "harness.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
using harness::GateCode;
using harness::SandboxBackend;
using harness::TaskStatus;

struct Host {
  harness::TokenFirewallConfig fcfg;
  std::unique_ptr<harness::TokenFirewall> fw;
  std::unique_ptr<harness::SisterLeafBus> bus;
  std::unique_ptr<harness::ForwardNudgeLoop> nudge;
  std::unique_ptr<harness::TaskArena> arena;
  std::unique_ptr<harness::WorktreeManager> worktrees;
  std::unique_ptr<harness::SandboxRunner> sandbox;
  std::mutex mu;
};

// --- tiny JSON readers -------------------------------------------------------
std::string jstr(const std::string& j, const std::string& key,
                 const std::string& dflt = {}) {
  std::string q = "\"" + key + "\"";
  auto p = j.find(q);
  if (p == std::string::npos) return dflt;
  p = j.find(':', p + q.size());
  if (p == std::string::npos) return dflt;
  p = j.find('"', p);
  if (p == std::string::npos) return dflt;
  std::string out;
  for (std::size_t i = p + 1; i < j.size(); ++i) {
    char c = j[i];
    if (c == '\\' && i + 1 < j.size()) {
      char n = j[++i];
      if (n == 'n') out += '\n';
      else if (n == 't') out += '\t';
      else out += n;
      continue;
    }
    if (c == '"') break;
    out += c;
  }
  return out;
}

long jlong(const std::string& j, const std::string& key, long dflt = 0) {
  std::string q = "\"" + key + "\"";
  auto p = j.find(q);
  if (p == std::string::npos) return dflt;
  p = j.find(':', p + q.size());
  if (p == std::string::npos) return dflt;
  return std::atol(j.c_str() + p + 1);
}

bool jbool(const std::string& j, const std::string& key, bool dflt = false) {
  std::string q = "\"" + key + "\"";
  auto p = j.find(q);
  if (p == std::string::npos) return dflt;
  p = j.find(':', p + q.size());
  if (p == std::string::npos) return dflt;
  auto t = j.substr(p + 1, 6);
  if (t.find("true") != std::string::npos) return true;
  if (t.find("false") != std::string::npos) return false;
  return dflt;
}

std::vector<std::string> jwords(const std::string& j, const std::string& key) {
  std::vector<std::string> out;
  std::string q = "\"" + key + "\"";
  auto p = j.find(q);
  if (p == std::string::npos) return out;
  p = j.find('[', p + q.size());
  if (p == std::string::npos) return out;
  auto e = j.find(']', p);
  if (e == std::string::npos) return out;
  std::string inner = j.substr(p + 1, e - p - 1);
  std::string cur;
  bool inQ = false;
  for (std::size_t i = 0; i < inner.size(); ++i) {
    char c = inner[i];
    if (c == '"' && (i == 0 || inner[i - 1] != '\\')) {
      inQ = !inQ;
      continue;
    }
    if (!inQ && (c == ',')) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    if (inQ) cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string jesc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}

// UTF-8-safe truncation: never split a multi-byte sequence. Caller passes
// capacity including NUL; we back up to the last ASCII or sequence start.
int emit(char* out, size_t out_len, int* trunc, const std::string& s) {
  if (!out || out_len == 0) return HARNESS_C_ERR;
  if (s.size() + 1 <= out_len) {
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    if (trunc) *trunc = 0;
    return HARNESS_C_OK;
  }
  std::size_t n = out_len - 1;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  std::memcpy(out, s.data(), n);
  out[n] = '\0';
  if (trunc) *trunc = 1;
  return HARNESS_C_OK;
}

std::string verdictName(harness::GateVerdict v) {
  using harness::GateVerdict;
  if (v == GateVerdict::Allow) return "allow";
  if (v == GateVerdict::Warn) return "warn";
  if (v == GateVerdict::Deny) return "deny";
  return "propose-only";
}

std::string codeName(GateCode c) {
  switch (c) {
    case GateCode::Ok: return "Ok";
    case GateCode::BudgetWarn: return "BudgetWarn";
    case GateCode::SpiralStop: return "SpiralStop";
    case GateCode::BudgetExceeded: return "BudgetExceeded";
    case GateCode::BreadthCap: return "BreadthCap";
    case GateCode::DailyCap: return "DailyCap";
    case GateCode::DepthFirebreak: return "DepthFirebreak";
    case GateCode::HomogeneousAssignment: return "HomogeneousAssignment";
    case GateCode::DestructiveProposeOnly: return "DestructiveProposeOnly";
    case GateCode::MaxRounds: return "MaxRounds";
    case GateCode::AtomicLocked: return "AtomicLocked";
    case GateCode::SplitOnce: return "SplitOnce";
    case GateCode::BusValueTooLarge: return "BusValueTooLarge";
    case GateCode::ArtifactsPending: return "ArtifactsPending";
    case GateCode::VerifyFail: return "VerifyFail";
    case GateCode::VerifyTimeout: return "VerifyTimeout";
    case GateCode::HarnessAbsent: return "HarnessAbsent";
    case GateCode::WorktreeMainDirty: return "WorktreeMainDirty";
    case GateCode::WorktreeStaleBase: return "WorktreeStaleBase";
    case GateCode::WorktreeLocked: return "WorktreeLocked";
    case GateCode::WorktreeBranchCheckedOut: return "WorktreeBranchCheckedOut";
    case GateCode::WorktreePathExists: return "WorktreePathExists";
    case GateCode::WorktreeMergeConflict: return "WorktreeMergeConflict";
    case GateCode::WorktreeUntrackedOnly: return "WorktreeUntrackedOnly";
    case GateCode::WorktreeGitignoreMissing: return "WorktreeGitignoreMissing";
  }
  return "Ok";
}
}  // namespace

struct harness_host_s : Host {};

extern "C" {

harness_host_t* harness_host_create(const char* config_json) {
  try {
    auto* h = new harness_host_s();
    std::string j = config_json ? config_json : "";
    h->fcfg.ledgerPath = jstr(j, "ledgerPath", "/tmp/harness-ledger.jsonl");
    std::string shm = jstr(j, "shmPath", "");
    std::string root = jstr(j, "repoRoot", ".");
    h->fw = harness::makeTokenFirewall(h->fcfg);
    h->bus = harness::makeSisterLeafBus(shm);
    h->nudge = harness::makeForwardNudgeLoop(harness::NudgeConfig{});
    h->arena = harness::makeTaskArena(*h->fw, *h->bus, h->fcfg.ledgerPath);
    h->worktrees = harness::makeWorktreeManager(root);
    h->sandbox = harness::makeSandboxRunner();
    return h;
  } catch (...) {
    return nullptr;
  }
}

void harness_host_destroy(harness_host_t* host) {
  try {
    delete static_cast<Host*>(host);
  } catch (...) {
  }
}

int harness_check_split(harness_host_t* host, const char* task_id,
                        int parent_depth, int breadth, int resplit_count,
                        int parent_atomic, int parent_already_split,
                        const char* worker_models_json, long tree_used,
                        long day_used, char* out, size_t out_len,
                        int* out_truncated) {
  if (!host) return HARNESS_C_ERR;
  try {
    std::lock_guard<std::mutex> l(static_cast<Host*>(host)->mu);
    Host* h = static_cast<Host*>(host);
    std::vector<std::string> models =
        jwords(worker_models_json ? worker_models_json : "", "models");
    auto d = h->fw->checkSplit(task_id ? task_id : "", parent_depth, breadth,
                               resplit_count, parent_atomic != 0,
                               parent_already_split != 0, models, tree_used,
                               day_used);
    std::string s = "{\"verdict\":\"" + verdictName(d.verdict) +
                    "\",\"code\":\"" + codeName(d.code) + "\",\"estimate\":" +
                    std::to_string(d.estimate) + ",\"message\":\"" +
                    jesc(d.message) + "\"}";
    return emit(out, out_len, out_truncated, s);
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

int harness_report(harness_host_t* host, const char* vector_json, char* out,
                   size_t out_len, int* out_truncated) {
  if (!host) return HARNESS_C_ERR;
  try {
    std::lock_guard<std::mutex> l(static_cast<Host*>(host)->mu);
    Host* h = static_cast<Host*>(host);
    std::string j = vector_json ? vector_json : "";
    harness::ResultVector v;
    v.taskId = jstr(j, "taskId");
    auto st = harness::ResultVector::parseStatus(jstr(j, "status", "success"));
    v.status = st ? *st : TaskStatus::Failed;
    v.summary = jstr(j, "summary");
    v.changedFiles = jwords(j, "changedFiles");
    v.gitDiffHash = jstr(j, "gitDiffHash");
    v.exportedState = {{"anchor", jstr(j, "anchor")}};
    if (v.exportedState["anchor"].empty()) v.exportedState.clear();
    std::string artifact = jstr(j, "checkArtifact", "exit=0");
    GateCode rc = h->arena->report(v, artifact);
    return emit(out, out_len, out_truncated,
                "{\"code\":\"" + codeName(rc) + "\"}");
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

int harness_bus_post(harness_host_t* host, const char* entry_json, char* out,
                     size_t out_len, int* out_truncated) {
  if (!host) return HARNESS_C_ERR;
  try {
    std::lock_guard<std::mutex> l(static_cast<Host*>(host)->mu);
    Host* h = static_cast<Host*>(host);
    std::string j = entry_json ? entry_json : "";
    harness::SisterLeafEntry e;
    e.taskId = jstr(j, "taskId");
    e.parentSessionId = jstr(j, "parentSessionId");
    e.namespace_ = jstr(j, "namespace");
    e.key = jstr(j, "key");
    e.value = jstr(j, "value");
    e.updatedBy = jstr(j, "updatedBy");
    GateCode rc = h->bus->publish(e);
    return emit(out, out_len, out_truncated,
                "{\"code\":\"" + codeName(rc) + "\"}");
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

int harness_sandbox_exec(harness_host_t* host, const char* req_json, char* out,
                         size_t out_len, int* out_truncated) {
  if (!host) return HARNESS_C_ERR;
  try {
    Host* h = static_cast<Host*>(host);
    std::string j = req_json ? req_json : "";
    harness::SandboxRunRequest req;
    req.worktreePath = jstr(j, "worktreePath", ".");
    req.argv = jwords(j, "argv");
    if (req.argv.empty()) req.argv = {"true"};
    req.timeoutMs = std::chrono::milliseconds(jlong(j, "timeoutMs", 300000));
    req.denyEgress = jbool(j, "denyEgress", true);
    auto r = h->sandbox->run(req);
    std::string backend =
        (r.backend == SandboxBackend::LinuxNamespaces) ? "linux-namespaces"
        : (r.backend == SandboxBackend::MacSandboxExec) ? "mac-sandbox-exec"
                                                        : "unisolated";
    std::string s = "{\"exitCode\":" + std::to_string(r.exitCode) +
                    ",\"durationMs\":" + std::to_string(r.durationMs) +
                    ",\"backend\":\"" + backend + "\",\"isolated\":" +
                    (r.isolated ? "true" : "false") + ",\"stdoutTail\":\"" +
                    jesc(r.stdoutTail) + "\",\"stderrTail\":\"" +
                    jesc(r.stderrTail) + "\"}";
    return emit(out, out_len, out_truncated, s);
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

int harness_worktree_acquire(harness_host_t* host, const char* team,
                             const char* task_id, int attempt,
                             const char* base_branch, int flat_layout, char* out,
                             size_t out_len, int* out_truncated) {
  if (!host) return HARNESS_C_ERR;
  try {
    std::lock_guard<std::mutex> l(static_cast<Host*>(host)->mu);
    Host* h = static_cast<Host*>(host);
    auto layout = flat_layout ? harness::WorktreeLayout::Flat
                              : harness::WorktreeLayout::Team;
    auto [code, handle] = h->worktrees->acquire(
        team ? team : "", task_id ? task_id : "", attempt,
        base_branch ? base_branch : "main", layout);
    std::string s = "{\"code\":\"" + codeName(code) + "\",\"branch\":\"" +
                    jesc(handle.branch) + "\",\"path\":\"" +
                    jesc(handle.path) + "\",\"baseSha\":\"" +
                    jesc(handle.baseSha) + "\"}";
    return emit(out, out_len, out_truncated, s);
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

int harness_worktree_release(harness_host_t* host, const char* team,
                             const char* task_id, int merged, char* out,
                             size_t out_len, int* out_truncated) {
  if (!host) return HARNESS_C_ERR;
  try {
    std::lock_guard<std::mutex> l(static_cast<Host*>(host)->mu);
    Host* h = static_cast<Host*>(host);
    GateCode rc = h->worktrees->release(team ? team : "", task_id ? task_id : "",
                                        merged != 0);
    return emit(out, out_len, out_truncated,
                "{\"code\":\"" + codeName(rc) + "\"}");
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

int harness_backend_label(char* out, size_t out_len, int* out_truncated) {  try {
    std::string git =
#ifdef HARNESS_USE_LIBGIT2
        "libgit2-native";
#else
        "shell-git";
#endif
    std::string sb =
#if HARNESS_OS_LINUX
        "+linux-namespaces";
#elif HARNESS_OS_MACOS
        "+sandbox-exec";
#else
        "+unisolated";
#endif
    return emit(out, out_len, out_truncated, git + sb);
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

// Jev routing: daemon holds NO API key. harness_jev_status is a static
// policy report; harness_jev_route reuses the TokenFirewall B^D budget
// gate (checkSplit, non-atomic, resplit 0, hetero pair) BEFORE any
// Jev/LLM dispatch. Deny => dispatchAllowed=false => caller must fall
// back locally without a remote call.
int harness_jev_status(char* out, size_t out_len, int* out_truncated) {
  try {
    return emit(out, out_len, out_truncated,
                "{\"available\":true,\"mode\":\"ts-sidecar-only\","
                "\"keyInDaemon\":false,\"budgetGate\":\"B^D pre-dispatch\","
                "\"policy\":\"deny-means-no-dispatch\"}");
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

int harness_jev_route(harness_host_t* host, int parent_depth, int breadth,
                      long tree_used, long day_used, char* out, size_t out_len,
                      int* out_truncated) {
  if (!host) return HARNESS_C_ERR;
  if (breadth < 1) breadth = 1;
  if (parent_depth < 0) parent_depth = 0;
  try {
    std::lock_guard<std::mutex> l(static_cast<Host*>(host)->mu);
    Host* h = static_cast<Host*>(host);
    // Hard budget check FIRST: same normative gate as check_split, with a
    // hetero model pair so the assignment rule also applies. No Jev/LLM
    // dispatch may happen when this gate denies.
    auto d = h->fw->checkSplit("jev-route", parent_depth, breadth,
                               /*resplitCount=*/0, /*parentAtomic=*/false,
                               /*parentAlreadySplit=*/false,
                               {"model-a", "model-b"}, tree_used, day_used);
    bool allowed = (d.verdict != harness::GateVerdict::Deny);
    std::string s = "{\"verdict\":\"" + verdictName(d.verdict) +
                    "\",\"code\":\"" + codeName(d.code) + "\",\"estimate\":" +
                    std::to_string(d.estimate) + ",\"dispatchAllowed\":" +
                    (allowed ? "true" : "false") + ",\"message\":\"" +
                    jesc(d.message) + "\"}";
    return emit(out, out_len, out_truncated, s);
  } catch (...) {
    return HARNESS_C_ERR;
  }
}

}  // extern "C"
