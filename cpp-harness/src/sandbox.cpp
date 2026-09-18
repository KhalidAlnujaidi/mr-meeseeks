// sandbox.cpp — WorktreeManager (shell git, optional libgit2) + SandboxRunner.
//
//   WorktreeManager: git worktree isolation per spec §B / §D.3. Primary
//     implementation shells out to `git` (always available wherever the
//     harness runs). When libgit2 is present at configure time
//     (HARNESS_USE_LIBGIT2), base-SHA resolution uses libgit2 with shell
//     fallback — same observable behavior on both paths.
//   SandboxRunner: verify/test execution inside the worktree per spec §D.4.
//     Linux: namespaces/seccomp isolation (bubblewrap when present, else
//     unshare); macOS: sandbox-exec best-effort; elsewhere: unisolated
//     fallback with isolated=false recorded. Only exit codes gate `proposed`;
//     isolation failure never blocks it.
//
// Portable C++20 + POSIX (fork/exec/poll). No other dependencies.

#include "harness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(HARNESS_USE_LIBGIT2)
#include <git2.h>
namespace harness {
namespace git_native {
std::string resolveBaseShaNative(const std::string& repoRoot,
                                 const std::string& rev);
int addWorktreeNative(const std::string& repoRoot, const std::string& branch,
                      const std::string& path, const std::string& baseSha,
                      std::string& errOut);
int diffWorktreeNative(const std::string& repoRoot, const std::string& wtPath,
                       const std::string& baseSha, std::string& statOut,
                       std::string& namesOut, std::string& errOut);
}  // namespace git_native
}  // namespace harness
#endif

#if HARNESS_OS_LINUX
#include <poll.h>
#include <fcntl.h>
#elif HARNESS_OS_MACOS
#include <poll.h>
#include <fcntl.h>
#endif

namespace harness {
namespace {

// ---------------------------------------------------------------------------
// Shell helper: run argv, capture stdout+exit code (stderr merged optional)
// ---------------------------------------------------------------------------

struct CmdResult {
  int exitCode = -1;
  std::string out;
};

CmdResult runCmd(const std::vector<std::string>& argv,
                 const std::string& cwd = {},
                 bool mergeStderr = true) {
  // Build a safely-quoted shell command: every arg single-quoted.
  std::string cmd;
  if (!cwd.empty()) cmd += "cd '" + cwd + "' && ";
  for (const auto& a : argv) {
    cmd += "'";
    for (char c : a) {
      if (c == '\'')
        cmd += "'\\''";
      else
        cmd += c;
  }
    cmd += "' ";
  }
  if (mergeStderr) cmd += "2>&1";
  std::array<char, 4096> buf{};
  std::string out;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) return {-1, {}};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
    out += buf.data();
  }
  int rc = ::pclose(pipe);
  int code = -1;
  if (WIFEXITED(rc)) code = WEXITSTATUS(rc);
  return {code, out};
}

bool exeExists(const std::string& name) {
  CmdResult r = runCmd({"command", "-v", name});
  return r.exitCode == 0 && !r.out.empty();
}

/// True when stderr smells like a sandbox-wrapper setup failure (not the
/// payload's own exit 1): bwrap uid-map/loopback denials, sandbox-exec
/// profile errors. Guards the degrade-to-unisolated retry so a real
/// exit-1 from the workload never gets re-run.
bool looksLikeSandboxError(const std::string& stderrTail) {
  for (const char* probe : {"bwrap:", "uid map", "RTM_NEWADDR",
                            "sandbox-exec", "sandbox apply"}) {
    if (stderrTail.find(probe) != std::string::npos) return true;
  }
  return false;
}

std::string trim(const std::string& s) {
  std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  std::size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

/// Prune long output per spec §E.2.2: over 8192 chars keep head 4096 + tail.
std::string pruneTail(const std::string& s) {
  if (s.size() <= PRUNE_THRESHOLD_CHARS) return s;
  return s.substr(0, PRUNE_HEAD_CHARS) + "\n...[pruned " +
         std::to_string(s.size() - PRUNE_HEAD_CHARS - PRUNE_TAIL_CHARS) +
         " chars]...\n" +
         s.substr(s.size() - PRUNE_TAIL_CHARS);
}

std::string jsonEscapeLocal(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else
      out += c;
  }
  return out;
}

// Resolve <repo>/<rev> to a full SHA. Native libgit2 revparse when linked,
// else shell. Same observable behavior on both paths.
std::string resolveBaseSha(const std::string& repoRoot, const std::string& rev) {
#if defined(HARNESS_USE_LIBGIT2)
  {
    std::string sha = git_native::resolveBaseShaNative(repoRoot, rev);
    if (!sha.empty()) return sha;
  }
#endif
  CmdResult r = runCmd({"git", "-C", repoRoot, "rev-parse", rev});
  if (r.exitCode != 0) return {};
  return trim(r.out);
}

}  // namespace

// ---------------------------------------------------------------------------
// WorktreeManager — shell-git implementation
// ---------------------------------------------------------------------------

class WorktreeManagerImpl : public WorktreeManager {
 public:
  explicit WorktreeManagerImpl(std::string repoRoot)
      : repoRoot_(std::move(repoRoot)) {}

  std::pair<GateCode, WorktreeHandle> acquire(
      const std::string& team, const std::string& taskId, int attempt,
      const std::string& baseBranch = "main",
      WorktreeLayout layout = WorktreeLayout::Team) override {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = team + "/" + taskId;
    if (registry_.count(key)) return {GateCode::WorktreeLocked, {}};

    // WT_MAIN_DIRTY: tracked modifications in the main checkout block acquire.
    CmdResult st = runCmd({"git", "-C", repoRoot_, "status", "--porcelain"});
    if (st.exitCode == 0) {
      std::istringstream lines(st.out);
      std::string line;
      while (std::getline(lines, line)) {
        if (line.size() >= 2 && line[0] == '?' && line[1] == '?') continue;
        if (!trim(line).empty()) return {GateCode::WorktreeMainDirty, {}};
      }
    }

    WorktreeHandle h;
    h.taskId = taskId;
    h.attempt = attempt;
    h.baseBranch = baseBranch;
    if (layout == WorktreeLayout::Team) {
      h.branch = "task/" + team + "/" + taskId + "-a" + std::to_string(attempt);
      h.path = repoRoot_ + "/.agent-teams/" + team + "/worktrees/" + taskId +
               "-a" + std::to_string(attempt);
    } else {
      // Flat layout collides across teams and needs the .gitignore gate.
      CmdResult gi = runCmd({"git", "-C", repoRoot_, "check-ignore", "-q",
                             ".worktrees/placeholder"});
      (void)gi;
      std::ifstream ignore(repoRoot_ + "/.gitignore");
      bool ok = false;
      if (ignore) {
        std::string l;
        while (std::getline(ignore, l)) {
          if (trim(l) == ".worktrees/") {
            ok = true;
            break;
          }
        }
      }
      if (!ok) return {GateCode::WorktreeGitignoreMissing, {}};
      h.branch = "agent/" + taskId + "-a" + std::to_string(attempt);
      h.path = repoRoot_ + "/.worktrees/" + taskId + "-a" + std::to_string(attempt);
    }

    if (std::filesystem::exists(h.path)) return {GateCode::WorktreePathExists, {}};
    CmdResult br = runCmd({"git", "-C", repoRoot_, "show-ref", "--verify",
                           "--quiet", "refs/heads/" + h.branch});
    if (br.exitCode == 0) return {GateCode::WorktreeBranchCheckedOut, {}};

    h.baseSha = resolveBaseSha(repoRoot_, baseBranch);
    if (h.baseSha.empty()) return {GateCode::WorktreeStaleBase, {}};

    std::filesystem::create_directories(
        std::filesystem::path(h.path).parent_path());
#if defined(HARNESS_USE_LIBGIT2)
    // Native path: git_worktree_add via C bindings (no shell fork).
    {
      std::string err;
      int rc = git_native::addWorktreeNative(repoRoot_, h.branch, h.path,
                                             h.baseSha, err);
      if (rc != 0) return {GateCode::WorktreeLocked, {}};
      (void)err;
    }
#else
    CmdResult add = runCmd({"git", "-C", repoRoot_, "worktree", "add", "-b",
                            h.branch, h.path, h.baseSha});
    if (add.exitCode != 0) return {GateCode::WorktreeLocked, {}};
#endif
    h.state = WorktreeState::Active;
    registry_[key] = h;
    return {GateCode::Ok, h};
  }

  std::pair<GateCode, ProposeResult> propose(const std::string& team,
                                             const std::string& taskId) override {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = team + "/" + taskId;
    auto it = registry_.find(key);
    if (it == registry_.end() || it->second.state != WorktreeState::Active) {
      return {GateCode::WorktreeLocked, {}};
    }
    WorktreeHandle& h = it->second;

    // D.4 pipeline runs INSIDE the worktree first. Red suite => registry
    // stays active, V_VERIFY_FAIL, no patch (BRAIN never reviews red code).
    auto runner = makeSandboxRunner();
    VerifyReport vr = runner->verify(h.path, taskId, /*docsOnly=*/false,
                                     /*brainWaived=*/false);
    bool waived = vr.verdict == VerifyVerdict::WaivedDocsOnly ||
                  vr.verdict == VerifyVerdict::WaivedHarnessAbsent;
    if (vr.verdict != VerifyVerdict::Pass && !waived) {
      if (vr.verdict == VerifyVerdict::Timeout) {
        return {GateCode::VerifyTimeout, {}};
      }
      return {GateCode::VerifyFail, {}};
    }

    CmdResult diff = runCmd(
        {"git", "-C", h.path, "diff", h.baseSha, "--", ".", ":!patches"});
    CmdResult stat =
        runCmd({"git", "-C", h.path, "diff", "--stat", h.baseSha, "--", "."});
    CmdResult por = runCmd({"git", "-C", h.path, "status", "--porcelain"});
#if defined(HARNESS_USE_LIBGIT2)
    // Native diff replaces the shell output when linked: stat + names from
    // git_diff_tree_to_workdir (no fork). Porcelain stays shell (untracked
    // listing is already covered natively, but keep one shell call minimal).
    {
      std::string nstat, nnames, nerr;
      if (git_native::diffWorktreeNative(repoRoot_, h.path, h.baseSha, nstat,
                                         nnames, nerr) == 0 &&
          !nstat.empty()) {
        stat.out = nstat;
        if (!nnames.empty()) por.out += nnames;
        diff.out = nstat;  // patch text falls back to shell diff below
      }
      (void)nerr;
    }
#endif
    if (trim(diff.out).empty() && trim(por.out).empty()) {
      return {GateCode::WorktreeUntrackedOnly, {}};
    }
    std::filesystem::create_directories(repoRoot_ + "/patches");
    const std::string patchFile = repoRoot_ + "/patches/" + taskId + ".patch";
    {
      std::ofstream p(patchFile);
      p << diff.out;
    }
    ProposeResult res;
    res.patchFile = patchFile;
    res.stat = stat.out;
    res.statusPorcelain = por.out;
    res.verifyJsonPath = vr.verifyJsonPath;
    h.state = WorktreeState::Proposed;
    return {GateCode::Ok, res};
  }

  std::pair<GateCode, std::string> mergeBack(const std::string& team,
                                             const std::string& taskId,
                                             MergeStrategy strategy) override {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = team + "/" + taskId;
    auto it = registry_.find(key);
    if (it == registry_.end() || it->second.state != WorktreeState::Proposed) {
      return {GateCode::WorktreeLocked, {}};
    }
    WorktreeHandle& h = it->second;
    const std::string patchFile = repoRoot_ + "/patches/" + taskId + ".patch";
    std::string mergedSha;
    if (strategy == MergeStrategy::Apply) {
      CmdResult check =
          runCmd({"git", "-C", repoRoot_, "apply", "--check", patchFile});
      if (check.exitCode != 0) return {GateCode::WorktreeMergeConflict, {}};
      CmdResult ap = runCmd({"git", "-C", repoRoot_, "apply", patchFile});
      if (ap.exitCode != 0) return {GateCode::WorktreeMergeConflict, {}};
      mergedSha = trim(runCmd({"git", "-C", repoRoot_, "rev-parse", "HEAD"}).out);
    } else {
      std::vector<std::string> argv = {"git", "-C", repoRoot_, "merge",
                                       strategy == MergeStrategy::MergeFf
                                           ? "--ff-only"
                                           : "--no-ff",
                                       h.branch};
      // --no-ff needs a message; --ff-only takes none.
      std::vector<std::string> full = argv;
      if (strategy == MergeStrategy::MergeNoFf) {
        full.push_back("-m");
        full.push_back("Merge " + h.branch);
      }
      CmdResult mg = runCmd(full);
      if (mg.exitCode != 0) {
        runCmd({"git", "-C", repoRoot_, "merge", "--abort"});
        return {GateCode::WorktreeMergeConflict, {}};
      }
      mergedSha = trim(runCmd({"git", "-C", repoRoot_, "rev-parse", "HEAD"}).out);
    }
    h.state = WorktreeState::Merged;
    return {GateCode::Ok, mergedSha};
  }

  GateCode release(const std::string& team, const std::string& taskId,
                   bool merged, bool keepBranch = false) override {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = team + "/" + taskId;
    auto it = registry_.find(key);
    if (it == registry_.end()) return GateCode::WorktreeLocked;
    WorktreeHandle h = it->second;
    runCmd({"git", "-C", repoRoot_, "worktree", "remove", "--force", h.path});
    runCmd({"git", "-C", repoRoot_, "worktree", "prune"});
    if (!keepBranch) {
      runCmd({"git", "-C", repoRoot_, "branch", "-D", h.branch});
    }
    h.state = merged ? WorktreeState::Merged : WorktreeState::Discarded;
    registry_.erase(it);  // tombstone: path/branch gone, state terminal
    return GateCode::Ok;
  }

  std::vector<WorktreeHandle> list(const std::string& team) override {
    std::lock_guard<std::mutex> lock(mu_);
    // Join the registry with `git worktree list --porcelain`: paths are
    // ALWAYS resolved via list()/registry, never string concatenation.
    CmdResult wl = runCmd({"git", "-C", repoRoot_, "worktree", "list", "--porcelain"});
    std::set<std::string> livePaths;
    if (wl.exitCode == 0) {
      std::istringstream lines(wl.out);
      std::string line;
      while (std::getline(lines, line)) {
        if (line.rfind("worktree ", 0) == 0) {
          livePaths.insert(trim(line.substr(9)));
        }
      }
    }
    std::vector<WorktreeHandle> out;
    for (const auto& [key, h] : registry_) {
      if (key.rfind(team + "/", 0) != 0) continue;
      WorktreeHandle view = h;
      if (!livePaths.count(h.path) && h.state == WorktreeState::Active) {
        view.state = WorktreeState::Stale;
      }
      out.push_back(view);
    }
    return out;
  }

 private:
  std::string repoRoot_;
  std::mutex mu_;  // serializes mergeBack per spec §B.4
  std::unordered_map<std::string, WorktreeHandle> registry_;
};

// ---------------------------------------------------------------------------
// SandboxRunner — fork/exec with timeout; platform isolation dispatch
// ---------------------------------------------------------------------------

class SandboxRunnerImpl : public SandboxRunner {
 public:
  SandboxBackend backend() const override {
#if HARNESS_OS_LINUX
    if (exeExists("bwrap")) return SandboxBackend::LinuxNamespaces;
    return SandboxBackend::Unisolated;  // H2: no wrapper => no isolation claim
#elif HARNESS_OS_MACOS
    if (exeExists("sandbox-exec")) return SandboxBackend::MacSandboxExec;
    return SandboxBackend::Unisolated;
#else
    return SandboxBackend::Unisolated;
#endif
  }

  SandboxRunResult run(const SandboxRunRequest& req) override {
    auto t0 = std::chrono::steady_clock::now();
    SandboxRunResult res;
    res.backend = backend();

    std::vector<std::string> argv = req.argv;
    bool wantSandbox = req.denyEgress;
    bool sandboxed = false;  // true once a sandbox wrapper is prepended
#if HARNESS_OS_LINUX
    // Prefer bubblewrap; else plain fork with unshare(CLONE_NEWNET) attempted
    // by a wrapper (best-effort: failure degrades, never blocks).
    if (wantSandbox && exeExists("bwrap")) {
      std::vector<std::string> wrapped = {
          "bwrap", "--unshare-net", "--die-with-parent", "--bind",
          req.worktreePath, req.worktreePath, "--chdir", req.worktreePath};
      wrapped.insert(wrapped.end(), argv.begin(), argv.end());
      argv = std::move(wrapped);
      wantSandbox = false;  // bwrap IS the isolation
      sandboxed = true;
    }
    // H2: isolated defaults FALSE; set true only after a sandbox wrapper
    // was actually prepended below (sandboxed == true). The post-exec
    // degrade path resets it on wrapper failure, and the final publish of
    // res.isolated(false) is what verify.json records.
    res.isolated = false;
#elif HARNESS_OS_MACOS
    if (wantSandbox && exeExists("sandbox-exec")) {
      // Best-effort macOS profile: deny egress + writes outside worktree.
      const std::string profile =
          "(version 1)(deny network-outbound)"
          "(allow process-exec)(allow file-read*)"
          "(allow file-write* (subpath \"" +
          req.worktreePath + "\")(subpath \"/tmp\")(subpath \"/private/tmp\"))";
      std::vector<std::string> wrapped = {"sandbox-exec", "-p", profile};
      wrapped.insert(wrapped.end(), argv.begin(), argv.end());
      argv = std::move(wrapped);
      res.isolated = true;
      sandboxed = true;
    } else {
      res.isolated = false;
      res.backend = SandboxBackend::Unisolated;
    }
#else
    res.isolated = false;
    res.backend = SandboxBackend::Unisolated;
#endif
    if (!wantSandbox && res.backend == SandboxBackend::Unisolated) {
      res.isolated = false;
    }

    // Fork/exec with piped stdout+stderr and a poll-loop timeout. Factored
    // as a lambda so a sandbox-apply failure (e.g. macOS sandbox-exec denied
    // in this environment, exit 71) retries UNISOLATED with isolated=false
    // recorded — isolation failure never blocks `proposed` (spec §D.4).
    auto execOnce = [&](const std::vector<std::string>& runArgv) {
    int outPipe[2], errPipe[2];
    // H3: close the first pair when the second pipe() fails (EMFILE storm),
    // then CLOEXEC all four so sandboxed children never inherit pipe fds.
    // dup2 clears CLOEXEC on the *target*, so stdout/stderr redirect survives.
    if (::pipe(outPipe) != 0) {
      res.exitCode = 127;
      return res;
    }
    if (::pipe(errPipe) != 0) {
      ::close(outPipe[0]);
      ::close(outPipe[1]);
      res.exitCode = 127;
      return res;
    }
    for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    pid_t pid = ::fork();
    if (pid < 0) {
      ::close(outPipe[0]);
      ::close(outPipe[1]);
      ::close(errPipe[0]);
      ::close(errPipe[1]);
      res.exitCode = 127;
      return res;
    }
    if (pid == 0) {
      // Child: pin CWD, wire pipes, exec.
      ::dup2(outPipe[1], STDOUT_FILENO);
      ::dup2(errPipe[1], STDERR_FILENO);
      ::close(outPipe[0]);
      ::close(outPipe[1]);
      ::close(errPipe[0]);
      ::close(errPipe[1]);
      if (!req.worktreePath.empty()) {
        if (::chdir(req.worktreePath.c_str()) != 0) ::_exit(127);
      }
      std::vector<char*> cargv;
      for (const auto& a : runArgv) cargv.push_back(const_cast<char*>(a.c_str()));
      cargv.push_back(nullptr);
      ::execvp(cargv[0], cargv.data());
      ::_exit(127);
    }
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    std::string so, se;
    int status = -1;
    bool timedOut = false;
    auto deadline = t0 + req.timeoutMs;
    struct pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
    char buf[4096];
    while (true) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timedOut = true;
        break;
      }
      int ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count());
      int pr = ::poll(fds, 2, std::min(ms, 50));
      if (pr > 0) {
        for (int i = 0; i < 2; ++i) {
          if (fds[i].revents & (POLLIN | POLLHUP)) {
            ssize_t n =
                ::read(fds[i].fd, buf, sizeof(buf));
            if (n > 0) {
              if (i == 0)
                so.append(buf, static_cast<std::size_t>(n));
              else
                se.append(buf, static_cast<std::size_t>(n));
            }
          }
        }
      }
      pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) break;
    }
    if (timedOut) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      res.exitCode = 124;  // timeout sentinel (V_VERIFY_TIMEOUT upstream)
    } else {
      // Drain remaining pipe bytes after exit.
      ssize_t n;
      while ((n = ::read(outPipe[0], buf, sizeof(buf))) > 0) {
        so.append(buf, static_cast<std::size_t>(n));
      }
      while ((n = ::read(errPipe[0], buf, sizeof(buf))) > 0) {
        se.append(buf, static_cast<std::size_t>(n));
      }
      res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    }
    ::close(outPipe[0]);
    ::close(errPipe[0]);
    auto t1 = std::chrono::steady_clock::now();
    res.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                         .count();
    res.stdoutTail = pruneTail(so);
    res.stderrTail = pruneTail(se);
    return res;
    };
    SandboxRunResult first = execOnce(argv);
    // H2: flip isolated=true ONLY here — a wrapper was prepended AND ran.
    // The degrade path below resets it on wrapper failure.
    if (sandboxed) {
      first.isolated = true;
      if (res.backend == SandboxBackend::Unisolated) {
        first.backend = SandboxBackend::LinuxNamespaces;
      }
    }
    // Sandbox-wrapper failures must degrade, never block: bwrap exits 1 with
    // "setting up uid map: Permission denied" / "loopback: Failed
    // RTM_NEWADDR" when unprivileged user namespaces are locked down
    // (enigma: ubuntu 24.04, kernel 7.0, no setuid bwrap), and macOS
    // sandbox-exec apply failure exits 71/72. Any of these while wrapped
    // retries the raw argv unisolated with isolated=false recorded.
    // Only exit codes gate `proposed` (spec §D.4).
    bool wrapperFailed =
        sandboxed && (first.exitCode == 1 || first.exitCode == 71 ||
                      first.exitCode == 72 || first.exitCode == 127);
    if (wrapperFailed && looksLikeSandboxError(first.stderrTail)) {
      SandboxRunResult retry = execOnce(req.argv);
      retry.backend = SandboxBackend::Unisolated;
      retry.isolated = false;
      return retry;
    }
    return first;
  }

  VerifyReport verify(const std::string& worktreePath,
                      const std::string& taskId, bool docsOnly,
                      bool brainWaived) override {
    VerifyReport rep;
    // Docs-only fast path: md-only diff + BRAIN waiver line required.
    if (docsOnly) {
      rep.verdict = brainWaived ? VerifyVerdict::WaivedDocsOnly
                                : VerifyVerdict::Fail;
      rep.verifyJsonPath = writeVerifyJson(worktreePath, taskId, rep);
      return rep;
    }
    detectHarnesses(worktreePath, rep.harnesses);
    if (rep.harnesses.empty()) {
      // Harness-absent: waiver path, never silent-pass.
      rep.verdict = brainWaived ? VerifyVerdict::WaivedHarnessAbsent
                                : VerifyVerdict::Fail;
      rep.verifyJsonPath = writeVerifyJson(worktreePath, taskId, rep);
      return rep;
    }
    bool allZero = true;
    for (VerifyHarness h : rep.harnesses) {
      SandboxRunRequest req;
      req.worktreePath = worktreePath;
      req.timeoutMs = VERIFY_TIMEOUT_MS;
      req.argv = harnessArgv(worktreePath, h);
      if (req.argv.empty()) continue;
      SandboxRunResult r = run(req);
      rep.exitCodes.push_back(r.exitCode);
      rep.durationMs += r.durationMs;
      rep.isolated = r.isolated;
      if (r.exitCode != 0) allZero = false;
      if (r.exitCode == 124) {  // killed at timeoutMs
        rep.verdict = VerifyVerdict::Timeout;
        rep.verifyJsonPath = writeVerifyJson(worktreePath, taskId, rep);
        return rep;
      }
    }
    rep.verdict = allZero ? VerifyVerdict::Pass : VerifyVerdict::Fail;
    rep.verifyJsonPath = writeVerifyJson(worktreePath, taskId, rep);
    return rep;
  }

 private:
  static void detectHarnesses(const std::string& wt,
                              std::vector<VerifyHarness>& out) {
    namespace fs = std::filesystem;
    // npm test, else `npm run verify` when `test` is absent.
    if (fs::exists(wt + "/package.json")) {
      std::ifstream pj(wt + "/package.json");
      std::string content((std::istreambuf_iterator<char>(pj)),
                          std::istreambuf_iterator<char>());
      if (content.find("\"test\"") != std::string::npos) {
        out.push_back(VerifyHarness::NpmTest);
      } else if (content.find("\"verify\"") != std::string::npos) {
        out.push_back(VerifyHarness::NpmVerify);
      }
    }
    if (fs::exists(wt + "/pytest.ini") || fs::exists(wt + "/pyproject.toml") ||
        fs::exists(wt + "/setup.cfg")) {
      out.push_back(VerifyHarness::Pytest);
    }
    if (fs::exists(wt + "/.eslintrc.js") || fs::exists(wt + "/.eslintrc.json") ||
        fs::exists(wt + "/.eslintrc.cjs") || fs::exists(wt + "/eslint.config.js")) {
      out.push_back(VerifyHarness::Eslint);
    }
    if (fs::exists(wt + "/ruff.toml") || fs::exists(wt + "/.ruff.toml")) {
      out.push_back(VerifyHarness::Ruff);
    }
  }

  static std::vector<std::string> harnessArgv(const std::string& wt,
                                              VerifyHarness h) {
    switch (h) {
      case VerifyHarness::NpmTest: return {"npm", "test", "--prefix", wt};
      case VerifyHarness::NpmVerify:
        return {"npm", "run", "verify", "--prefix", wt};
      case VerifyHarness::Pytest: return {"pytest", "-q", wt};
      case VerifyHarness::Eslint: return {"npx", "eslint", wt};
      case VerifyHarness::Ruff: return {"ruff", "check", wt};
      case VerifyHarness::None: return {};
    }
    return {};
  }

  static std::string writeVerifyJson(const std::string& wt,
                                     const std::string& taskId,
                                     const VerifyReport& rep) {
    std::string path = wt + "/verify.json";
    std::ostringstream o;
    o << "{\"task_id\":\"" << jsonEscapeLocal(taskId) << "\",\"verdict\":"
      << static_cast<int>(rep.verdict) << ",\"exit_codes\":[";
    for (std::size_t i = 0; i < rep.exitCodes.size(); ++i) {
      if (i) o << ",";
      o << rep.exitCodes[i];
    }
    o << "],\"duration_ms\":" << rep.durationMs << ",\"isolated\":"
      << (rep.isolated ? "true" : "false") << "}";
    std::ofstream f(path);
    if (f) f << o.str() << "\n";
    return path;
  }
};

// ---------------------------------------------------------------------------
// §12 Factories (sandbox side)
// ---------------------------------------------------------------------------

std::unique_ptr<WorktreeManager> makeWorktreeManager(
    const std::string& repoRoot) {
  return std::make_unique<WorktreeManagerImpl>(repoRoot);
}

std::unique_ptr<SandboxRunner> makeSandboxRunner() {
  return std::make_unique<SandboxRunnerImpl>();
}

}  // namespace harness
