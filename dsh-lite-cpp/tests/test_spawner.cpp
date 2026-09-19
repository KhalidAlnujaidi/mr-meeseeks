// test_spawner.cpp — Milestone 2 acceptance: fork/exec isolation, scrubbed
// env, workspace pinning, sanitized capture, watchdog SIGKILL.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "dshlite/spawner.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}
}  // namespace

int main() {
  using namespace dshlite;
  SwarmSpawner sp;

  // 1. Basic exec + capture + sanitization (colors stripped for Brain).
  {
    SpawnOptions o;
    o.argv = {"printf", "\\033[31mhi\\033[0m \\001\\n"};
    o.timeout = std::chrono::seconds(10);
    SpawnResult r = sp.spawn(o);
    check(r.exitCode == 0, "printf exits 0");
    check(r.sanitizedStdout == "hi \n", "stdout sanitized for Brain");
    check(r.workspaceDir.rfind("/tmp/meeseeks_", 0) == 0,
          "workspace is /tmp/meeseeks_<uuid>");
    check(std::filesystem::is_directory(r.workspaceDir),
          "workspace retained for forensics");
    SwarmSpawner::cleanup(r.workspaceDir);
    check(!std::filesystem::exists(r.workspaceDir), "cleanup removes workspace");
  }

  // 2. Scrubbed env: hostile parent env NEVER leaks.
  {
    ::setenv("MEESEEKS_HOST_SECRET", "super-secret-parent-token", 1);
    ::setenv("OPENROUTER_API_KEY", "sk-or-host-key-must-not-leak", 1);
    SpawnOptions o;
    o.argv = {"env"};  // prints its whole environment
    o.timeout = std::chrono::seconds(10);
    SpawnResult r = sp.spawn(o);
    check(r.sanitizedStdout.find("MEESEEKS_HOST_SECRET") == std::string::npos,
          "parent secret not in child env");
    check(r.sanitizedStdout.find("sk-or-host-key") == std::string::npos,
          "host API key not in child env");
    check(r.sanitizedStdout.find("PATH=") == std::string::npos,
          "host PATH not inherited");
    SwarmSpawner::cleanup(r.workspaceDir);
  }

  // 3. Explicit allowlist: authorized vars DO reach the child.
  {
    SpawnOptions o;
    o.argv = {"env"};
    o.allowedEnv = {{"TASK_TOKEN", "explicitly-authorized"}};
    o.timeout = std::chrono::seconds(10);
    SpawnResult r = sp.spawn(o);
    check(r.sanitizedStdout.find("TASK_TOKEN=explicitly-authorized") !=
              std::string::npos,
          "allowlisted var reaches child");
    SwarmSpawner::cleanup(r.workspaceDir);
  }

  // 4. CWD pinned to workspace + scope files symlinked in.
  {
    const std::string scope = "/tmp/meeseeks_scope_probe.txt";
    {
      std::ofstream f(scope);
      f << "scope-data\n";
    }
    SpawnOptions o;
    o.argv = {"sh", "-c", "pwd; cat scope_probe 2>/dev/null || cat ./*probe*"};
    o.scopeFiles = {scope};
    o.timeout = std::chrono::seconds(10);
    SpawnResult r = sp.spawn(o);
    check(r.sanitizedStdout.find(r.workspaceDir) != std::string::npos,
          "child CWD is the isolated workspace");
    check(r.sanitizedStdout.find("scope-data") != std::string::npos,
          "scope file visible inside workspace");
    SwarmSpawner::cleanup(r.workspaceDir);
    std::filesystem::remove(scope);
  }

  // 5. Watchdog: infinite loop SIGKILLed, sentinel 124, sanitized output.
  {
    SpawnOptions o;
    o.argv = {"sh", "-c",
              "printf '\\033[32mspinning\\033[0m\\n'; sleep 300"};
    o.timeout = std::chrono::milliseconds(800);
    auto t0 = std::chrono::steady_clock::now();
    SpawnResult r = sp.spawn(o);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    check(r.timedOut && r.exitCode == 124, "hang SIGKILLed, exit 124");
    check(ms < 10000, "watchdog fires near deadline, not at sleep end");
    check(r.sanitizedStdout == "spinning\n", "pre-kill output still sanitized");
    SwarmSpawner::cleanup(r.workspaceDir);
  }

  // 6. Exec failure surfaces 127, never throws.
  {
    SpawnOptions o;
    o.argv = {"/nonexistent/meeseeks-binary-xyz"};
    o.timeout = std::chrono::seconds(5);
    SpawnResult r = sp.spawn(o);
    check(r.exitCode == 127, "missing binary -> 127");
    SwarmSpawner::cleanup(r.workspaceDir);
  }

  std::cout << (failures == 0 ? "SPAWNER PASS\n" : "SPAWNER FAIL\n");
  return failures == 0 ? 0 : 1;
}
