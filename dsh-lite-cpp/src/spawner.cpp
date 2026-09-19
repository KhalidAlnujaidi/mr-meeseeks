// spawner.cpp — Module 2: Swarm spawner (fork/execve, scrubbed env, watchdog).

#include "dshlite/spawner.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>

#include "dshlite/sanitizer.hpp"

namespace dshlite {
namespace {

constexpr int TIMEOUT_SENTINEL = 124;

// Fixed PATH for argv[0] lookup (never inherits the host's PATH).
constexpr const char* kDefaultPath =
    "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";

std::string makeWorkspace() {
  std::random_device rd;
  std::array<unsigned, 4> r{rd(), rd(), rd(), rd()};
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/tmp/meeseeks_%08x%08x%08x%08x", r[0],
                r[1], r[2], r[3]);
  return std::string(buf);
}

std::string baseNameOf(const std::string& p) {
  auto pos = p.find_last_of('/');
  return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

// Resolve argv[0] against kDefaultPath when it has no slash.
std::string resolveBin(const std::string& prog) {
  if (prog.find('/') != std::string::npos) return prog;
  std::string path = kDefaultPath;
  std::size_t i = 0;
  while (i <= path.size()) {
    auto j = path.find(':', i);
    std::string dir =
        path.substr(i, j == std::string::npos ? j : j - i);
    std::string cand = dir + "/" + prog;
    if (::access(cand.c_str(), X_OK) == 0) return cand;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return prog;  // let execve fail loudly (exit 127 path)
}

void setCloexec(int fd) {
  int f = ::fcntl(fd, F_GETFD);
  if (f >= 0) ::fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

}  // namespace

void SwarmSpawner::cleanup(const std::string& workspaceDir) noexcept {
  if (workspaceDir.empty()) return;
  // Contain blast radius: only ever remove our own /tmp/meeseeks_* dirs.
  if (workspaceDir.rfind("/tmp/meeseeks_", 0) != 0) return;
  try {
    std::filesystem::remove_all(workspaceDir);
  } catch (...) {
  }
}

SpawnResult SwarmSpawner::spawn(const SpawnOptions& opt) {
  auto t0 = std::chrono::steady_clock::now();
  SpawnResult res;

  if (opt.argv.empty()) {
    res.exitCode = 127;
    res.sanitizedStderr = "spawn: empty argv";
    return res;
  }

  // 1. Workspace: /tmp/meeseeks_<uuid>/, scope files symlinked in.
  res.workspaceDir = makeWorkspace();
  try {
    std::filesystem::create_directories(res.workspaceDir);
    std::size_t n = 0;
    for (const auto& src : opt.scopeFiles) {
      std::string link =
          res.workspaceDir + "/" + baseNameOf(src) + (n ? "_" + std::to_string(n) : "");
      ++n;
      std::error_code ec;
      std::filesystem::create_symlink(src, link, ec);  // best-effort
    }
  } catch (...) {
    res.exitCode = 127;
    res.sanitizedStderr = "spawn: workspace setup failed";
    return res;
  }

  // 2. Pipes (CLOEXEC so exec'd children never inherit read ends).
  int outPipe[2], errPipe[2];
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
  setCloexec(outPipe[0]);
  setCloexec(outPipe[1]);
  setCloexec(errPipe[0]);
  setCloexec(errPipe[1]);

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[0]);
    ::close(errPipe[1]);
    res.exitCode = 127;
    res.sanitizedStderr = "spawn: fork failed";
    return res;
  }

  if (pid == 0) {
    // --- Child: hostile-untrusted, minimal surface ---
    ::dup2(outPipe[1], STDOUT_FILENO);
    ::dup2(errPipe[1], STDERR_FILENO);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[0]);
    ::close(errPipe[1]);
    if (::chdir(res.workspaceDir.c_str()) != 0) ::_exit(127);

    // Scrubbed env: ONLY explicitly authorized entries.
    std::vector<std::string> envStrings;
    envStrings.reserve(opt.allowedEnv.size());
    for (const auto& [k, v] : opt.allowedEnv) {
      if (!k.empty() && k.find('=') == std::string::npos)
        envStrings.push_back(k + "=" + v);
    }
    std::vector<char*> envp;
    for (auto& s : envStrings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::string bin = resolveBin(opt.argv[0]);
    std::vector<char*> cargv;
    cargv.reserve(opt.argv.size() + 1);
    cargv.push_back(bin.data());
    for (std::size_t i = 1; i < opt.argv.size(); ++i)
      cargv.push_back(const_cast<char*>(opt.argv[i].c_str()));
    cargv.push_back(nullptr);

    ::execve(bin.c_str(), cargv.data(), envp.data());
    ::_exit(127);  // execve failed
  }

  // --- Parent: watchdog capture loop ---
  ::close(outPipe[1]);
  ::close(errPipe[1]);
  ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
  ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

  std::string rawOut, rawErr;
  int status = -1;
  bool timedOut = false;
  auto deadline = t0 + opt.timeout;
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
          ssize_t n = ::read(fds[i].fd, buf, sizeof(buf));
          if (n > 0) {
            if (i == 0)
              rawOut.append(buf, static_cast<std::size_t>(n));
            else
              rawErr.append(buf, static_cast<std::size_t>(n));
          }
        }
      }
    }
    pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
  }

  if (timedOut) {
    ::kill(pid, SIGKILL);  // structural watchdog: freeze => SIGKILL
    ::waitpid(pid, &status, 0);
    res.timedOut = true;
    res.exitCode = TIMEOUT_SENTINEL;
  } else {
    ssize_t n;
    while ((n = ::read(outPipe[0], buf, sizeof(buf))) > 0)
      rawOut.append(buf, static_cast<std::size_t>(n));
    while ((n = ::read(errPipe[0], buf, sizeof(buf))) > 0)
      rawErr.append(buf, static_cast<std::size_t>(n));
    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  }
  ::close(outPipe[0]);
  ::close(errPipe[0]);

  // 3. Context firebreak: Brain sees ONLY sanitized text.
  res.sanitizedStdout = sanitize(rawOut);
  res.sanitizedStderr = sanitize(rawErr);
  auto t1 = std::chrono::steady_clock::now();
  res.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                       .count();
  return res;
}

}  // namespace dshlite
