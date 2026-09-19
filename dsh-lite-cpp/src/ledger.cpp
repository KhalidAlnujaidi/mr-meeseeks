// ledger.cpp — Module 4: ledger.jsonl v2 emitter. See ledger.hpp.

#include "dshlite/ledger.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "dshlite/sanitizer.hpp"

namespace dshlite {

std::string utcNowIso() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<long>(ms.count()));
  return buf;
}

LedgerWriter::LedgerWriter(const std::string& path) : path_(path) {
  fd_ = ::open(path.c_str(), O_APPEND | O_CREAT | O_WRONLY, 0644);
  if (fd_ < 0)
    throw std::runtime_error("ledger: cannot open " + path + " for append");
}

LedgerWriter::~LedgerWriter() {
  if (fd_ >= 0) ::close(fd_);
}

std::string LedgerWriter::serialize(const LedgerEvent& ev, const std::string& tsIso) {
  nlohmann::json j;
  j["type"] = ev.type;
  j["schema"] = kLedgerSchema;
  j["task_id"] = ev.taskId;
  if (!ev.parentId.empty()) j["parent_id"] = ev.parentId;
  j["depth"] = ev.depth;
  j["role"] = ev.role;
  j["model"] = ev.model;
  j["endpoint"] = ev.endpoint;
  j["ts"] = tsIso;

  nlohmann::json cost;
  cost["prompt_tokens"] = ev.promptTokens;
  cost["completion_tokens"] = ev.completionTokens;
  if (ev.latencyMs >= 0) cost["latency_ms"] = ev.latencyMs;
  // F11: ttft null when not measured (non-streaming) — never fabricated.
  cost["ttft_ms"] = (ev.ttftMs >= 0) ? nlohmann::json(ev.ttftMs) : nlohmann::json(nullptr);
  if (ev.tokPerSec > 0.0) cost["tok_per_sec"] = ev.tokPerSec;  // F12: omit on no signal
  j["cost"] = std::move(cost);

  if (ev.hasCache) j["cache"] = {{"warm", ev.cacheWarm}};  // F5: best-effort only
  if (!ev.verdict.empty()) j["verdict"] = ev.verdict;
  if (!ev.gate.empty()) j["gate"] = ev.gate;
  if (!ev.code.empty()) j["code"] = ev.code;
  if (ev.nudgeDepth >= 0) j["nudge_depth"] = ev.nudgeDepth;
  if (ev.resplitCount >= 0) j["resplit_count"] = ev.resplitCount;
  if (!ev.detail.empty()) j["detail"] = sanitize(ev.detail);  // Module 1, always

  return j.dump();  // single line: nlohmann emits no raw newlines in strings
}

void LedgerWriter::append(const LedgerEvent& ev) {
  std::string line = serialize(ev, utcNowIso());
  line.push_back('\n');
  std::lock_guard<std::mutex> lock(mu_);  // F15
  size_t off = 0;
  while (off < line.size()) {
    const ssize_t n = ::write(fd_, line.data() + off, line.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("ledger: write failed on " + path_);
    }
    off += static_cast<size_t>(n);
  }
}

LedgerEvent LedgerWriter::fromRouted(const std::string& type, const std::string& role,
                                     const std::string& taskId, int depth,
                                     const RoutedResponse& r) {
  LedgerEvent ev;
  ev.type = type;
  ev.role = role;
  ev.taskId = taskId;
  ev.depth = depth;
  ev.model = r.modelId;
  ev.endpoint = r.servedBy;
  ev.promptTokens = r.response.usage.promptTokens;
  ev.completionTokens = r.response.usage.completionTokens;
  ev.latencyMs = r.response.latencyMs;
  ev.ttftMs = r.response.ttftMs;
  ev.tokPerSec = decodeTokPerSec(r.response);  // F13 decode-window rate
  return ev;
}

}  // namespace dshlite
