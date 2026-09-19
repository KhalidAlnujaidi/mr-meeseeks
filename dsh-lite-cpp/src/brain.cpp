// brain.cpp — Module 3b: Executive Iteration Loop + node judge hook.

#include "dshlite/brain.hpp"

#include <filesystem>
#include <map>
#include <string>

#include "dshlite/spawner.hpp"

namespace dshlite {
namespace {

// Parse "key=value" token value (to end of line / whitespace).
std::string tokenAfter(const std::string& text, const std::string& key) {
  auto p = text.find(key);
  if (p == std::string::npos) return {};
  p += key.size();
  std::size_t e = text.find_first_of(" \t\r\n", p);
  return text.substr(p, e == std::string::npos ? e : e - p);
}

std::string firstLineWith(const std::string& text, const std::string& key) {
  auto p = text.find(key);
  if (p == std::string::npos) return {};
  auto ls = text.rfind('\n', p);
  auto le = text.find('\n', p);
  return text.substr(ls == std::string::npos ? 0 : ls + 1,
                     (le == std::string::npos ? text.size() : le) -
                         (ls == std::string::npos ? 0 : ls + 1));
}

JudgeVerdict fallback(const std::string& detail) {
  JudgeVerdict v;
  v.action = JudgeAction::Escalate;
  v.confidence = 0.0;
  v.fallback = true;
  v.detail =
      "judge unavailable — ESCALATED TO USER (no worker spawned). " + detail;
  return v;
}

std::string cap(std::string s, std::size_t n = 1000) {
  if (s.size() > n) s.resize(n);
  return s;
}

}  // namespace

JudgeHook makeNodeJudgeHook(const std::string& loopTs,
                            const std::map<std::string, std::string>& allowedEnv,
                            std::chrono::milliseconds timeout) {
  return [loopTs, allowedEnv, timeout](const std::string& criterion) {
    // Never throws: every failure mode degrades to fallback-escalate.
    try {
      if (criterion.empty()) return fallback("empty criterion");
      SwarmSpawner spawner;
      SpawnOptions opt;
      // Resolve the loop entry relative to the repo root. The binary may
      // run from dsh-lite-cpp/build; the loop lives one level up. If the
      // caller passed an absolute path, use it verbatim.
      std::string loop = loopTs;
      if (!loop.empty() && loop[0] != '/' &&
          !std::filesystem::exists(loop)) {
        std::string up = std::string("../") + loop;
        if (std::filesystem::exists(up)) loop = up;
      }
      opt.argv = {"node", loop, "judge", criterion};
      opt.allowedEnv = allowedEnv;
      opt.timeout = timeout;
      SpawnResult r = spawner.spawn(opt);
      // Ephemeral workspace: judge output is already extracted into
      // sanitized strings below; the directory must not outlive the call.
      const std::string judgeWs = r.workspaceDir;
      const bool judgeTimedOut = r.timedOut;
      const int judgeExit = r.exitCode;
      if (judgeTimedOut) {
        SwarmSpawner::cleanup(judgeWs);
        return fallback("judge worker timed out after " +
                        std::to_string(opt.timeout.count()) + "ms (SIGKILLed)");
      }
      if (judgeExit != 0) {
        std::string err = cap(r.sanitizedStderr, 300);
        SwarmSpawner::cleanup(judgeWs);
        return fallback("judge worker exited " + std::to_string(judgeExit) +
                        (err.empty() ? "" : ": " + err));
      }
      const std::string out = r.sanitizedStdout;  // already firewalled
      SwarmSpawner::cleanup(judgeWs);
      const std::string action = tokenAfter(out, "action=");
      const std::string route = tokenAfter(out, "route=");
      double conf = 0.0;
      try {
        const std::string cs = tokenAfter(out, "conf=");
        if (!cs.empty()) conf = std::stod(cs);
      } catch (...) {
      }
      const bool markedFallback = out.find("fallback") != std::string::npos;

      JudgeVerdict v;
      v.route = route;
      v.confidence = conf;
      v.fallback = markedFallback || action.empty();
      v.detail = cap(firstLineWith(out, "route=") + " / " +
                     firstLineWith(out, "action="));
      if (v.fallback || action.empty() || action.rfind("escalate", 0) == 0) {
        v.action = JudgeAction::Escalate;
        v.fallback = true;
        v.detail =
            "judge low-confidence/fallback — ESCALATED TO USER (no worker "
            "spawned). " +
            v.detail;
      } else if (action == "act:do_direct") {
        v.action = JudgeAction::DoDirect;
      } else if (action == "act:split_once") {
        v.action = JudgeAction::SplitOnce;
      } else if (action == "strict_verify") {
        v.action = JudgeAction::StrictVerify;
      } else if (action == "spiral_stop") {
        v.action = JudgeAction::SpiralStop;
      } else {
        v.action = JudgeAction::Escalate;
        v.fallback = true;
        v.detail = "unrecognized judge action '" + cap(action, 60) +
                   "' — ESCALATED TO USER (no worker spawned). " + v.detail;
      }
      return v;
    } catch (const std::exception& e) {
      return fallback(std::string("hook exception: ") + e.what());
    } catch (...) {
      return fallback("hook threw (unknown)");
    }
  };
}

BrainLoop::BrainLoop(ILlmPoster& llm, JudgeHook judge)
    : llm_(llm), judge_(std::move(judge)) {}

void BrainLoop::setSystemPrompt(const std::string& s) {
  // Strategy state only; newest system prompt wins (single entry).
  for (auto& m : history_) {
    if (m.role == "system") {
      m.content = s;
      return;
    }
  }
  history_.insert(history_.begin(), Message{"system", s});
}

std::string BrainLoop::turn(const std::string& userInput) {
  history_.push_back(Message{"user", userInput});
  LlmResponse r = llm_.post(history_);
  history_.push_back(Message{"assistant", r.content});
  return r.content;
}

JudgeVerdict BrainLoop::gateDelegation(const std::string& criterion) {
  // The Atomicity Judge runs BEFORE any delegation. Transport/timeout
  // failure escalates to the user — never unverified execution, never lock.
  JudgeVerdict v;
  try {
    v = judge_(criterion);
  } catch (const std::exception& e) {
    return fallback(std::string("judge hook threw: ") + e.what());
  } catch (...) {
    return fallback("judge hook threw (unknown)");
  }
  if (v.fallback && v.action != JudgeAction::Escalate)
    v.action = JudgeAction::Escalate;
  if ((v.action == JudgeAction::Escalate ||
       v.action == JudgeAction::SpiralStop) &&
      v.detail.empty()) {
    v.detail = v.action == JudgeAction::Escalate
                   ? "judge declined — ESCALATED TO USER (no worker spawned)"
                   : "SPIRAL risk — lineage stopped, report awaited";
  }
  return v;
}

}  // namespace dshlite
