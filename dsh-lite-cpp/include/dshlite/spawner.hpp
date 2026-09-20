#pragma once
// spawner.hpp — Module 2: Low-Level Process Isolation (SRS Milestone 2).
//
// Workers are hostile and untrusted:
//  - true native processes via POSIX fork()/execve(), detached memory.
//  - scrubbed envp[]: ONLY SpawnOptions::allowedEnv reaches the child.
//    Host tokens / API keys NEVER leak unless explicitly listed there.
//  - dedicated workspace <temp-dir>/golem/ws_<uuid>/ per task (temp
//    boundary via std::filesystem, override GOLEM_WORKSPACE_ROOT); only
//    listed scopeFiles are symlinked in; the child CWD is pinned there.
//  - stdout/stderr captured through CLOEXEC pipes, sanitized (Module 1)
//    before the Brain ever sees them.
//  - watchdog thread SIGKILLs the child past opt.timeout (default 60 s).

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace dshlite {

struct SpawnOptions {
  std::vector<std::string> argv;  ///< exact argv; argv[0] is the binary
  std::vector<std::string> scopeFiles;  ///< host paths symlinked into workspace
  /// The ONLY environment the child receives (KEY -> VALUE). Empty by
  /// default: pass COLI_API_KEY / TYPESAFE_API_KEY here only when
  /// the task explicitly requires it.
  std::map<std::string, std::string> allowedEnv;
  std::chrono::milliseconds timeout{60000};  ///< watchdog SIGKILL deadline
};

struct SpawnResult {
  int exitCode = 127;  ///< 124 = watchdog timeout sentinel
  bool timedOut = false;
  std::string sanitizedStdout;  ///< Brain-safe (Module 1), truncated
  std::string sanitizedStderr;  ///< Brain-safe (Module 1), truncated
  long durationMs = 0;
  /// Retained after return for forensics; caller removes via cleanup().
  std::string workspaceDir;
};

class SwarmSpawner {
 public:
  SpawnResult spawn(const SpawnOptions& opt);
  /// Best-effort recursive remove; never throws.
  static void cleanup(const std::string& workspaceDir) noexcept;
};

}  // namespace dshlite
