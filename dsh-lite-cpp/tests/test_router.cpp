// test_router.cpp — Gap 1 acceptance (docs/colibri-roadmap.md G1.1–G1.5):
// role-based dispatch, worker->worker fallback escalation (never brain),
// single-family pool config warning (G1.2), verbatim model-id 404 ->
// next family, config validation, probe isolation, thread-safe fan-out.
// Transport is loopback-only via in-process httplib stub servers that
// mimic distinct `coli serve` engines (one served --model-id each).

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "dshlite/router.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

bool throwsWith(const std::function<void()>& fn, const std::string& needle) {
  try {
    fn();
  } catch (const std::exception& e) {
    return std::string(e.what()).find(needle) != std::string::npos;
  } catch (...) {
    return false;
  }
  return false;
}

// A colibri-faithful stub engine: 404 unless body.model == served id.
struct StubEngine {
  httplib::Server srv;
  std::thread th;
  int port = -1;
  std::string servedId;
  std::atomic<long> hits{0};

  void start(const std::string& id, int portHint, const std::string& reply) {
    servedId = id;
    srv.Post("/v1/chat/completions",
             [this, reply](const httplib::Request& req, httplib::Response& res) {
               hits.fetch_add(1);
               if (req.body.find("\"" + servedId + "\"") == std::string::npos) {
                 res.status = 404;
                 res.set_content(R"({"error":{"type":"model_not_found"}})",
                                 "application/json");
                 return;
               }
               res.set_content(
                   R"({"choices":[{"message":{"content":")" + reply +
                       R"("}}],"usage":{"prompt_tokens":10,"completion_tokens":5,)"
                       R"("total_tokens":15}})",
                   "application/json");
             });
    for (int p = portHint; p < portHint + 40; ++p) {
      httplib::Server probe;
      if (!probe.bind_to_port("127.0.0.1", p)) continue;
      port = p;
      break;
    }
    if (port == -1) return;
    th = std::thread([this] { srv.listen("127.0.0.1", port); });
    for (int i = 0; i < 100 && !srv.is_running(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  void stop() {
    if (port == -1) return;
    srv.stop();
    if (th.joinable()) th.join();
  }
  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  }
};

dshlite::EngineEntry entry(const StubEngine& e) {
  dshlite::EngineEntry en;
  en.endpoint = e.url();
  en.modelId = e.servedId;
  en.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
  en.timeout = std::chrono::seconds(5);
  return en;
}
}  // namespace

int main() {
  using namespace dshlite;

  // 0. Family derivation (pure, no sockets) — naive dash-split would
  // count qwen3.8-flash-next and qwen3.6 as two families; they are one.
  check(deriveFamily("glm-5.3-flash-colibri") == "glm", "deriveFamily glm");
  check(deriveFamily("glm-5.2-colibri") == "glm", "deriveFamily glm-5.2 same family");
  check(deriveFamily("qwen3.8-flash-next-colibri") == "qwen", "deriveFamily qwen3.8");
  check(deriveFamily("qwen3.6-colibri") == "qwen", "deriveFamily qwen3.6 same family");
  check(deriveFamily("deepseek-v4-colibri") == "deepseek", "deriveFamily deepseek");
  check(deriveFamily("olmoe-colibri") == "olmoe", "deriveFamily olmoe");
  check(roleFromName("worker") == Role::Worker && roleName(Role::Brain) == std::string("brain"),
        "role name round-trip");
  check(throwsWith([] { roleFromName("captain"); }, "unknown role"), "bad role name throws");

  // Three distinct stub engines: brain (glm), worker A (qwen), worker B
  // (olmoe). Different ports, different served model-ids.
  StubEngine brain, wkA, wkB;
  brain.start("glm-5.2-colibri", 18100, "brain here");
  wkA.start("qwen3.8-flash-next-colibri", 18150, "worker A");
  wkB.start("olmoe-colibri", 18200, "worker B");
  check(brain.port != -1 && wkA.port != -1 && wkB.port != -1 &&
            brain.port != wkA.port && wkA.port != wkB.port,
        "three stub engines on distinct loopback ports");
  if (brain.port == -1 || wkA.port == -1 || wkB.port == -1) {
    std::cout << "ROUTER FAIL\n";
    return 1;
  }

  const std::vector<Message> msgs = {{"user", "do the atom"}};

  // 1. G1.1 role-based dispatch: each role lands on its own engine,
  // model-id sent verbatim (stub 404s otherwise — a hit proves the pair).
  {
    RouterConfig cfg;
    cfg.brain = {entry(brain)};
    cfg.worker = {entry(wkA), entry(wkB)};
    cfg.verifier = {entry(wkB), entry(wkA)};
    ModelRouter r(cfg);
    check(r.warnings().empty(), "heterogeneous config => zero warnings");

    auto b = r.post(Role::Brain, msgs);
    check(b.response.content == "brain here" && b.servedBy == brain.url() &&
              brain.hits == 1,
          "brain role dispatches to brain engine only");
    auto w = r.post(Role::Worker, msgs);
    check(w.response.content == "worker A" && w.servedBy == wkA.url() &&
              wkA.hits == 1,
          "worker role dispatches to worker pool (first entry)");
    auto v = r.post(Role::Verifier, msgs);
    check(v.response.content == "worker B" && v.servedBy == wkB.url() &&
              wkB.hits == 1,
          "verifier role dispatches to verifier pool order");
    check(w.attempts.size() == 1 && w.attempts[0].outcome == "ok",
          "attempt log records the serving entry");
    check(r.requestCount() == 3 && r.totalUsage().totalTokens == 45,
          "pooled token totals aggregate across clients");
  }

  // 2. G1.3 worker->worker fallback: worker A refuses its own model-id
  // (simulate a dead/stalled engine by pointing entry A at a CLOSED
  // port) => router falls to worker B, never to the brain.
  {
    RouterConfig cfg;
    cfg.brain = {entry(brain)};
    EngineEntry deadA = entry(wkA);
    deadA.endpoint = "http://127.0.0.1:1/v1/chat/completions";  // closed port
    deadA.modelId = "qwen3.8-flash-next-colibri";
    cfg.worker = {deadA, entry(wkB)};
    cfg.verifier = {entry(wkB), entry(wkA)};
    ModelRouter r(cfg);

    const long brainBefore = brain.hits.load();
    auto w = r.post(Role::Worker, msgs);
    check(w.response.content == "worker B" && w.servedBy == wkB.url(),
          "dead worker falls through to next worker family");
    check(w.attempts.size() == 2 && w.attempts[0].outcome == "transport" &&
              w.attempts[1].outcome == "ok",
          "attempt log: transport failure then ok (audit trail for route lines)");
    check(brain.hits.load() == brainBefore,
          "F2: fallback NEVER touched the brain engine");
  }

  // 2b. Exhausted worker pool => throws (escalate); brain still untouched.
  {
    RouterConfig cfg;
    cfg.brain = {entry(brain)};
    EngineEntry dead1 = entry(wkA), dead2 = entry(wkB);
    dead1.endpoint = "http://127.0.0.1:1/v1/chat/completions";
    dead2.endpoint = "http://127.0.0.1:2/v1/chat/completions";
    cfg.worker = {dead1, dead2};
    cfg.verifier = {entry(wkB)};
    ModelRouter r(cfg);
    const long brainBefore = brain.hits.load();
    check(throwsWith([&] { r.post(Role::Worker, msgs); }, "pool exhausted"),
          "exhausted pool escalates with pool-exhausted error");
    check(brain.hits.load() == brainBefore, "F2: escalation never routed to brain");
  }

  // 2c. Verbatim model-id mismatch (F3): entry carries the WRONG id for
  // its engine => stub 404s => router falls to the next family, and the
  // 404 is classified in the attempt log (not a silent success).
  {
    RouterConfig cfg;
    cfg.brain = {entry(brain)};
    EngineEntry wrongId = entry(wkA);
    wrongId.modelId = "not-the-served-id";
    cfg.worker = {wrongId, entry(wkB)};
    cfg.verifier = {entry(wkB)};
    ModelRouter r(cfg);
    auto w = r.post(Role::Worker, msgs);
    check(w.servedBy == wkB.url() && w.attempts.size() == 2 &&
              w.attempts[0].outcome == "http-404",
          "F3: model-id mismatch surfaces as http-404 and falls through");
  }

  // 3. G1.2 single-family pool => config warning on BOTH leaf pools,
  // and qwen3.8 + qwen3.6 (two ids, ONE family) still warns.
  {
    RouterConfig cfg;
    cfg.brain = {entry(brain)};
    EngineEntry q2 = entry(wkB);
    q2.modelId = "qwen3.6-colibri";  // same family as wkA's qwen3.8
    cfg.worker = {entry(wkA), q2};
    cfg.verifier = {entry(wkA)};
    ModelRouter r(cfg);
    bool workerWarn = false, verifierWarn = false;
    for (const auto& w : r.warnings()) {
      if (w.find("worker pool") != std::string::npos) workerWarn = true;
      if (w.find("verifier pool") != std::string::npos) verifierWarn = true;
    }
    check(workerWarn, "G1.2: two qwen ids = one family => worker warning");
    check(verifierWarn, "G1.2: single-entry verifier pool => warning");
  }

  // 4. G1.1/G1.3 config validation (hard errors at load, not dispatch).
  {
    RouterConfig base;
    base.brain = {entry(brain)};
    base.worker = {entry(wkA), entry(wkB)};
    base.verifier = {entry(wkB)};

    check(throwsWith([&] { RouterConfig c = base; c.brain.clear(); ModelRouter r(c); },
                     "brain pool must not be empty"),
          "empty brain pool rejected");
    check(throwsWith([&] { RouterConfig c = base; c.worker.clear(); ModelRouter r(c); },
                     "worker pool must not be empty"),
          "empty worker pool rejected");
    check(throwsWith([&] {
            RouterConfig c = base;
            c.worker.push_back(entry(brain));  // brain endpoint in leaf pool
            ModelRouter r(c);
          },
                     "never a fallback target"),
          "F2: brain endpoint inside worker pool rejected at config time");
    check(throwsWith([&] {
            RouterConfig c = base;
            c.worker.push_back(entry(wkA));  // same endpoint twice
            ModelRouter r(c);
          },
                     "duplicate endpoint"),
          "duplicate endpoint within a pool rejected");
    check(throwsWith([&] {
            RouterConfig c = base;
            c.worker[0].modelId.clear();
            ModelRouter r(c);
          },
                     "empty modelId"),
          "F3: entry without model-id rejected (engine would 404)");
    check(throwsWith([&] {
            RouterConfig c = base;
            c.worker[0].endpoint = "ftp://127.0.0.1/x";
            ModelRouter r(c);
          },
                     "http://"),
          "bad scheme rejected at config time");
  }

  // 5. G1.5 probe: verifies each entry answers its own model-id, and
  // probe traffic never pollutes pooled totals.
  {
    RouterConfig cfg;
    cfg.brain = {entry(brain)};
    cfg.worker = {entry(wkA), entry(wkB)};
    cfg.verifier = {entry(wkB)};
    ModelRouter r(cfg);
    const long reqBefore = r.requestCount();
    auto pr = r.probe();
    check(pr.size() == 4, "probe covers every pooled entry");
    bool allOk = true;
    for (const auto& p : pr) allOk = allOk && p.ok;
    check(allOk, "all live engines answer their own model-id");
    check(r.requestCount() == reqBefore, "probe uses isolated clients (totals clean)");

    // One dead entry => probe reports it without throwing.
    RouterConfig cfg2 = cfg;
    EngineEntry dead = entry(wkA);
    dead.endpoint = "http://127.0.0.1:1/v1/chat/completions";
    cfg2.worker = {dead, entry(wkB)};
    ModelRouter r2(cfg2);
    auto pr2 = r2.probe();
    bool foundDead = false, othersOk = true;
    for (const auto& p : pr2) {
      if (p.endpoint == dead.endpoint) foundDead = !p.ok;
      else othersOk = othersOk && p.ok;
    }
    check(foundDead && othersOk, "probe flags dead entry, live entries still ok");
  }

  // 6. G1.4 thread-safe concurrent dispatch: N threads across roles,
  // pooled totals exact, no lost updates.
  {
    RouterConfig cfg;
    cfg.brain = {entry(brain)};
    cfg.worker = {entry(wkA), entry(wkB)};
    cfg.verifier = {entry(wkB), entry(wkA)};
    ModelRouter r(cfg);
    const long reqBefore = r.requestCount();
    const long tokBefore = r.totalUsage().totalTokens;
    constexpr int kThreads = 12;
    std::vector<std::future<RoutedResponse>> futs;
    for (int i = 0; i < kThreads; ++i) {
      const Role role = (i % 3 == 0)   ? Role::Brain
                        : (i % 3 == 1) ? Role::Worker
                                       : Role::Verifier;
      futs.push_back(r.postAsync(role, msgs));
    }
    bool allOk = true;
    for (auto& f : futs) allOk = allOk && !f.get().response.content.empty();
    check(allOk, "concurrent multi-role fan-out all served");
    check(r.requestCount() == reqBefore + kThreads &&
              r.totalUsage().totalTokens == tokBefore + 15L * kThreads,
          "G1.4: concurrent totals exact across pooled clients");
  }

  brain.stop();
  wkA.stop();
  wkB.stop();

  std::cout << (failures == 0 ? "ROUTER PASS\n" : "ROUTER FAIL\n");
  return failures == 0 ? 0 : 1;
}
