// router.cpp — Module 3c: Multi-Engine Local Router (Gap 1).
// See include/dshlite/router.hpp for the contract.

#include "dshlite/router.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <utility>

namespace dshlite {
namespace {

LlmConfig toConfig(const EngineEntry& e) {
  LlmConfig c;
  c.endpoint = e.endpoint;
  c.model = e.modelId;  // verbatim: engine 404s anything else
  c.maxTokens = e.maxTokens;
  c.timeout = e.timeout;
  c.apiKey = e.apiKey;
  c.apiKeyEnv = e.apiKeyEnv;
  c.stream = e.stream;  // G3.1: SSE ttft measurement, opt-in per entry
  c.stallNoBytesMs = e.stallNoBytesMs;  // G2.1/F6: no-bytes liveness cap
  return c;
}

std::string classify(const std::exception& ex) {
  const std::string m = ex.what();
  // G2.1/F6: the stall marker wins over "transport" — a no-bytes liveness
  // abort is its own ledger vocabulary, never lumped with generic failures.
  if (m.find("stall-no-bytes") != std::string::npos) return "stall-no-bytes";
  if (m.find("transport failure") != std::string::npos) return "transport";
  if (m.find("HTTP 404") != std::string::npos) return "http-404";
  if (m.find("HTTP 5") != std::string::npos) return "http-5xx";
  if (m.find("HTTP 4") != std::string::npos) return "http-4xx";
  if (m.find("malformed JSON") != std::string::npos) return "malformed";
  return "error";
}

void validateEntry(const EngineEntry& e, const char* pool) {
  if (e.endpoint.rfind("http://", 0) != 0 && e.endpoint.rfind("https://", 0) != 0)
    throw std::invalid_argument(
        std::string("router: ") + pool + " endpoint must start with http:// or https://: " + e.endpoint);
  if (e.modelId.empty())
    throw std::invalid_argument(
        std::string("router: ") + pool + " entry " + e.endpoint +
        " has empty modelId — the engine 404s anything but its verbatim --model-id (F3)");
}

std::string hostPortOf(const std::string& endpoint) {
  // authority between scheme and first '/' — identity for dup checks.
  auto slash = endpoint.find('/');
  auto scheme = endpoint.find("://");
  if (scheme == std::string::npos) return endpoint;
  auto start = scheme + 3;
  auto end = (slash == std::string::npos) ? endpoint.size() : slash;
  return endpoint.substr(start, end - start);
}

}  // namespace

const char* roleName(Role r) {
  switch (r) {
    case Role::Brain: return "brain";
    case Role::Worker: return "worker";
    case Role::Verifier: return "verifier";
  }
  return "unknown";
}

Role roleFromName(const std::string& name) {
  if (name == "brain") return Role::Brain;
  if (name == "worker") return Role::Worker;
  if (name == "verifier") return Role::Verifier;
  throw std::invalid_argument("router: unknown role '" + name + "'");
}

std::string deriveFamily(const std::string& modelId) {
  std::string s = modelId;
  const std::string suffix = "-colibri";
  if (s.size() > suffix.size() &&
      s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
    s.erase(s.size() - suffix.size());
  // Prefix up to the first digit, dash, or dot: glm-5.3-flash -> glm,
  // qwen3.8-flash-next -> qwen, deepseek-v4 -> deepseek, olmoe -> olmoe.
  size_t cut = s.find_first_of("0123456789-.");
  if (cut != std::string::npos && cut > 0) s.erase(cut);
  return s.empty() ? modelId : s;
}

ModelRouter::ModelRouter(RouterConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.brain.empty())
    throw std::invalid_argument("router: brain pool must not be empty");
  if (cfg_.worker.empty())
    throw std::invalid_argument("router: worker pool must not be empty");
  if (cfg_.verifier.empty())
    throw std::invalid_argument("router: verifier pool must not be empty");

  for (const auto& e : cfg_.brain) validateEntry(e, "brain");
  for (const auto& e : cfg_.worker) validateEntry(e, "worker");
  for (const auto& e : cfg_.verifier) validateEntry(e, "verifier");

  // G1.3: the brain endpoint is reachable only by role brain — reject
  // it appearing in any leaf pool at CONFIG time, not dispatch time.
  std::set<std::string> brainHosts;
  for (const auto& e : cfg_.brain) brainHosts.insert(hostPortOf(e.endpoint));
  for (const char* poolName : {"worker", "verifier"}) {
    const auto& pool = (poolName == std::string("worker")) ? cfg_.worker : cfg_.verifier;
    for (const auto& e : pool) {
      if (brainHosts.count(hostPortOf(e.endpoint)))
        throw std::invalid_argument(
            std::string("router: brain endpoint ") + e.endpoint + " appears in the " +
            poolName + " pool — the brain is never a fallback target (F2)");
    }
  }

  // Duplicate endpoint within a pool: same server twice is not a family.
  for (const auto& [pool, name] :
       {std::pair{&cfg_.worker, "worker"}, std::pair{&cfg_.verifier, "verifier"},
        std::pair{&cfg_.brain, "brain"}}) {
    std::set<std::string> seen;
    for (const auto& e : *pool) {
      if (!seen.insert(hostPortOf(e.endpoint)).second)
        throw std::invalid_argument(
            std::string("router: duplicate endpoint in ") + name + " pool: " + e.endpoint);
    }
  }

  // G1.2: heterogeneity machine-check — distinct FAMILIES, not ids.
  auto familyCount = [](const std::vector<EngineEntry>& pool) {
    std::set<std::string> fams;
    for (const auto& e : pool)
      fams.insert(e.family.empty() ? deriveFamily(e.modelId) : e.family);
    return fams.size();
  };
  if (familyCount(cfg_.worker) < 2)
    warnings_.push_back(
        "router: worker pool has < 2 distinct model families — correlated "
        "blindness risk (VISION.md constraint 2, G1.2)");
  if (familyCount(cfg_.verifier) < 2)
    warnings_.push_back(
        "router: verifier pool has < 2 distinct model families — worker and "
        "verifier may share failure modes (G1.2)");

  // Build clients once: [brain..., worker..., verifier...].
  brainN_ = cfg_.brain.size();
  workerN_ = cfg_.worker.size();
  const size_t total = brainN_ + workerN_ + cfg_.verifier.size();
  clients_.reserve(total);
  turnCounters_.reserve(total);
  for (const auto& e : cfg_.brain)
    clients_.push_back(std::make_unique<LlmClient>(toConfig(e)));
  for (const auto& e : cfg_.worker)
    clients_.push_back(std::make_unique<LlmClient>(toConfig(e)));
  for (const auto& e : cfg_.verifier)
    clients_.push_back(std::make_unique<LlmClient>(toConfig(e)));
  for (size_t i = 0; i < total; ++i)
    turnCounters_.push_back(std::make_unique<std::atomic<long>>(0));
}

const std::vector<EngineEntry>& ModelRouter::poolFor(Role role) const {
  switch (role) {
    case Role::Brain: return cfg_.brain;
    case Role::Worker: return cfg_.worker;
    case Role::Verifier: return cfg_.verifier;
  }
  throw std::invalid_argument("router: bad role");
}

LlmClient& ModelRouter::clientFor(Role role, size_t idx) {
  return *clients_.at(poolOffset_(role) + idx);
}

bool ModelRouter::velocityAborts(const EngineEntry& e, const LlmResponse& resp,
                                 long turnsSeen) {
  if (e.minTokPerSec <= 0.0) return false;           // floor disabled
  if (turnsSeen < e.warmupTurns) return false;       // F4 warmup exemption
  if (resp.usage.completionTokens <= 0) return false;  // F12: no signal
  return decodeTokPerSec(resp) < e.minTokPerSec;     // F13 decode window
}

RoutedResponse ModelRouter::post(Role role, const std::vector<Message>& messages) {
  const auto& pool = poolFor(role);
  RoutedResponse out;
  std::string lastErr;
  for (size_t i = 0; i < pool.size(); ++i) {
    RouteAttempt att{pool[i].endpoint, pool[i].modelId, "", ""};
    const long turnsSeen = turnCounters_[poolOffset_(role) + i]->load();
    try {
      LlmResponse resp = clientFor(role, i).post(messages);
      // G3.2: velocity floor AFTER the response — a too-slow engine is a
      // routing failure, so the pool falls through exactly like a stall.
      if (velocityAborts(pool[i], resp, turnsSeen)) {
        const double rate = decodeTokPerSec(resp);
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "velocity floor: %.3f tok/s < %.3f min (turns %ld, warmup %d)",
                      rate, pool[i].minTokPerSec, turnsSeen, pool[i].warmupTurns);
        turnCounters_[poolOffset_(role) + i]->fetch_add(1);  // F14: counted turn
        att.outcome = "velocity-floor";
        att.detail = buf;
        lastErr = buf;
        out.attempts.push_back(std::move(att));
        continue;  // F2: next worker family, never the brain
      }
      turnCounters_[poolOffset_(role) + i]->fetch_add(1);  // F14
      out.response = std::move(resp);
      att.outcome = "ok";
      out.attempts.push_back(std::move(att));
      out.servedBy = pool[i].endpoint;
      out.modelId = pool[i].modelId;
      return out;
    } catch (const LlmStallError& e) {
      // G2.1/F6: streaming liveness stall — typed, so the outcome never
      // depends on message text alone. Failed turns count (F14), then
      // fall through to the NEXT WORKER entry (F2: never the brain pool).
      turnCounters_[poolOffset_(role) + i]->fetch_add(1);
      att.outcome = "stall-no-bytes";
      att.detail = e.what();
      lastErr = e.what();
      out.attempts.push_back(std::move(att));
    } catch (const std::exception& e) {
      turnCounters_[poolOffset_(role) + i]->fetch_add(1);  // failed turns warm too
      att.outcome = classify(e);
      att.detail = e.what();
      lastErr = e.what();
      out.attempts.push_back(std::move(att));
      // F2: fall through to the NEXT entry in THIS pool (worker->worker,
      // different family). Never cross into the brain pool.
    }
  }
  throw std::runtime_error(
      std::string("router: ") + roleName(role) + " pool exhausted (" +
      std::to_string(pool.size()) + " entries failed; last: " + lastErr +
      ") — escalate; the brain pool is never a fallback target (F2)");
}

std::future<RoutedResponse> ModelRouter::postAsync(Role role,
                                                   std::vector<Message> messages) {
  return std::async(std::launch::async,
                    [this, role, m = std::move(messages)] { return post(role, m); });
}

std::vector<ModelRouter::ProbeResult> ModelRouter::probe() {
  std::vector<ProbeResult> results;
  const std::vector<Message> ping = {{"user", "ping"}};
  for (const auto* pool : {&cfg_.brain, &cfg_.worker, &cfg_.verifier}) {
    for (const auto& e : *pool) {
      ProbeResult pr{e.endpoint, e.modelId, false, ""};
      try {
        // Isolated client: probe traffic never touches pooled totals.
        LlmConfig c = toConfig(e);
        c.maxTokens = 1;
        c.timeout = std::min(c.timeout, std::chrono::milliseconds(5000));
        LlmClient probeClient(c);
        probeClient.post(ping);
        pr.ok = true;
        pr.detail = "answered its own model-id";
      } catch (const std::exception& ex) {
        pr.detail = ex.what();
      }
      results.push_back(std::move(pr));
    }
  }
  return results;
}

TokenUsage ModelRouter::totalUsage() const {
  TokenUsage t;
  for (const auto& c : clients_) {
    const auto u = c->totalUsage();
    t.promptTokens += u.promptTokens;
    t.completionTokens += u.completionTokens;
    t.totalTokens += u.totalTokens;
  }
  return t;
}

long ModelRouter::requestCount() const {
  long n = 0;
  for (const auto& c : clients_) n += c->requestCount();
  return n;
}

}  // namespace dshlite
