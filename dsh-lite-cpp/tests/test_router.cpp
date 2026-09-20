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
  std::string lastBody;  // G1.2/F60: request-body evidence (wire shape)

  void start(const std::string& id, int portHint, const std::string& reply) {
    servedId = id;
    srv.Post("/v1/chat/completions",
             [this, reply](const httplib::Request& req, httplib::Response& res) {
               hits.fetch_add(1);
               lastBody = req.body;
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

  // 11. G1.2 dual-family provisioning + role isolation + grammar wire
  // (F60): brain=olmoe, worker/verifier pools = {glm, olmoe2} — two
  // families per leaf pool, warnings clear; dispatch leaks nothing
  // across roles/families; a constrained worker call carries
  // response_format to the GLM engine (the grammar-capable family).
  {
    StubEngine olmoeBrain, glmWorker, olmoeWorker2;
    olmoeBrain.start("olmoe-colibri", 18300, "brain only");
    glmWorker.start("glm-5.2-colibri", 18310, "glm worker");
    olmoeWorker2.start("olmoe-colibri", 18320, "olmoe worker2");
    check(olmoeBrain.port != -1 && glmWorker.port != -1 && olmoeWorker2.port != -1,
          "11: dual-family stub engines up (olmoe brain, glm+olmoe leaves)");

    auto entry2 = [](StubEngine& s) {
      EngineEntry e;
      e.endpoint = s.url();
      e.modelId = s.servedId;
      e.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
      return e;
    };
    RouterConfig cfg;
    cfg.brain = {entry2(olmoeBrain)};
    cfg.worker = {entry2(glmWorker), entry2(olmoeWorker2)};
    cfg.verifier = {entry2(olmoeWorker2), entry2(glmWorker)};
    ModelRouter r(cfg);
    check(r.warnings().empty(),
          "11/G1.2: two-family leaf pools => single-family warnings CLEARED");

    const std::vector<Message> msgs = {{"user", "atom"}};
    // Role isolation, both directions, by hit counters (no leakage).
    auto b = r.post(Role::Brain, msgs);
    check(b.servedBy == olmoeBrain.url() && b.response.content == "brain only" &&
              glmWorker.hits == 0 && olmoeWorker2.hits == 0,
          "11: brain role -> olmoe brain ONLY (zero leaf-family hits)");
    auto w = r.post(Role::Worker, msgs);
    check(w.servedBy == glmWorker.url() && w.response.content == "glm worker" &&
              olmoeBrain.hits == 1,
          "11: worker role -> glm first entry; brain engine untouched (no leak)");
    auto v = r.post(Role::Verifier, msgs);
    check(v.servedBy == olmoeWorker2.url() && olmoeBrain.hits == 1 &&
              glmWorker.hits == 1,
          "11: verifier role -> its own pool order; still zero brain hits");

    // F60: grammar-constrained worker call — response_format reaches the
    // GLM engine's wire (glm family = grammar_payload capable).
    const auto rf = toolPayloadFormat({{"shell", nullptr}});
    auto cw = r.postConstrained(Role::Worker, msgs, rf);
    check(cw.servedBy == glmWorker.url() &&
              glmWorker.lastBody.find("response_format") != std::string::npos &&
              glmWorker.lastBody.find("json_schema") != std::string::npos,
          "11/F60: constrained dispatch carries response_format to the GLM engine");
    // And the brain wire NEVER sees response_format (no cross-role leak).
    r.post(Role::Brain, msgs);
    check(olmoeBrain.lastBody.find("response_format") == std::string::npos,
          "11: brain wire carries no response_format (role-pure)");

    olmoeBrain.stop();
    glmWorker.stop();
    olmoeWorker2.stop();
  }

  // 12. Mixed-lane routing (native C ABI integration, F81-F85): ABI
  // entries via an INJECTED fake factory — the core router links no
  // Colibri; the real factory (makeAbiBackendFactory) is covered by
  // test-abi's live section. The fake mirrors AbiClient's typed-failure
  // what() contracts so classify() exercises the real markers.
  {
    struct FakeAbiPoster : ILlmPoster {
      std::string reply, failMarker;  // empty marker => success
      std::atomic<long> hits{0};
      TokenUsage usage{};
      FakeAbiPoster(std::string r, std::string fm)
          : reply(std::move(r)), failMarker(std::move(fm)) {}
      LlmResponse post(const std::vector<Message>&) override {
        hits.fetch_add(1);
        if (!failMarker.empty()) throw std::runtime_error(failMarker);
        LlmResponse resp;
        resp.content = reply;
        resp.usage = usage;
        return resp;
      }
      TokenUsage totalUsage() const override { return usage; }
      long requestCount() const override { return hits.load(); }
    };
    std::vector<FakeAbiPoster*> built;
    auto fakeFactory = [&built](const std::string& reply,
                                const std::string& marker, long pt, long ct) {
      return RouterConfig::AbiFactory(
          [&built, reply, marker, pt, ct](const EngineEntry&) {
            auto p = std::make_unique<FakeAbiPoster>(reply, marker);
            p->usage.promptTokens = pt;
            p->usage.completionTokens = ct;
            p->usage.totalTokens = pt + ct;
            built.push_back(p.get());
            return std::unique_ptr<ILlmPoster>(std::move(p));
          });
    };
    auto abiEntry = [](const std::string& dir, const std::string& id) {
      EngineEntry e;
      e.backend = EngineBackend::InProcessAbi;
      e.modelDir = dir;
      e.modelId = id;
      return e;
    };

    StubEngine httpBrain, httpWorker;
    httpBrain.start("glm-5.2-colibri", 18400, "http brain");
    httpWorker.start("qwen3.6-colibri", 18410, "http worker");
    check(httpBrain.port != -1 && httpWorker.port != -1, "12: stubs up");

    // 12.1 ABI brain + HTTP worker/verifier: role dispatch crosses lanes.
    {
      RouterConfig cfg;
      cfg.brain = {abiEntry("/models/olmoe", "olmoe-leaf")};
      cfg.worker = {entry(httpWorker)};
      cfg.verifier = {entry(httpWorker)};
      cfg.abiFactory = fakeFactory("abi brain reply", "", 11, 7);
      ModelRouter r(cfg);
      auto b = r.post(Role::Brain, {{"user", "think"}});
      check(b.response.content == "abi brain reply" &&
                b.servedBy == "abi:/models/olmoe" && b.modelId == "olmoe-leaf",
            "12.1: ABI brain serves in-process; servedBy = abi:<modelDir> (F81)");
      check(b.attempts.size() == 1 && b.attempts[0].outcome == "ok" &&
                b.attempts[0].endpoint == "abi:/models/olmoe",
            "12.1: attempt log carries ABI identity");
      auto w = r.post(Role::Worker, {{"user", "work"}});
      check(w.servedBy == httpWorker.url() && w.response.content == "http worker",
            "12.1: HTTP worker lane unaffected by ABI brain");
      // F83: router aggregation spans both lanes (11+7 abi, 10+5 http stub).
      check(r.totalUsage().totalTokens == 33 && r.requestCount() == 2,
            "12.1/F83: totalUsage aggregates ABI + HTTP posters");
      // F82: probe reports ABI entries as resident, no second engine open.
      auto probes = r.probe();
      check(probes.size() == 3 && probes[0].ok &&
                probes[0].detail.find("resident") != std::string::npos &&
                probes[0].endpoint == "abi:/models/olmoe",
            "12.1/F82: ABI probe = resident-since-construction (never faked)");
      httpBrain.stop();
      httpWorker.stop();
    }

    // 12.2 Mixed fallback: ABI worker fails on the firewall marker ->
    // falls through to the HTTP worker (F2 seamless across lanes).
    {
      StubEngine wk2;
      wk2.start("olmoe-colibri", 18420, "http backup");
      // F2 brain isolation: the brain stub must NOT double as a worker
      // entry (config-time law), so the fallback target is a distinct
      // stub from the brain.
      StubEngine br2;
      br2.start("glm-5.2-colibri", 18430, "brain2");
      RouterConfig cfg;
      cfg.brain = {entry(br2)};
      cfg.worker = {abiEntry("/models/olmoe", "olmoe-leaf"), entry(wk2)};
      cfg.verifier = {entry(wk2)};
      cfg.abiFactory = fakeFactory("", "wall-clock firewall tripped during decode loop (step 3)", 0, 0);
      ModelRouter r(cfg);
      auto w = r.post(Role::Worker, {{"user", "work"}});
      check(w.servedBy == wk2.url() && w.response.content == "http backup",
            "12.2/F2: ABI worker firewall abort falls through to HTTP worker");
      check(w.attempts.size() == 2 && w.attempts[0].outcome == "abi-cancelled" &&
                w.attempts[0].endpoint == "abi:/models/olmoe" &&
                w.attempts[1].outcome == "ok",
            "12.2: attempt trail = abi-cancelled then ok (typed classification)");
      br2.stop();
      wk2.stop();
    }

    // 12.3 context-overflow marker classifies as its own outcome and
    // escalates when the pool is ABI-only.
    {
      StubEngine br3;
      br3.start("glm-5.2-colibri", 18440, "brain3");
      RouterConfig cfg;
      cfg.brain = {entry(br3)};
      cfg.worker = {abiEntry("/models/olmoe", "olmoe-leaf")};
      cfg.verifier = {abiEntry("/models/qwen", "qwen3.6-colibri")};
      cfg.abiFactory = fakeFactory("", "context overflow: need 9000 tokens, adapter max 4096", 0, 0);
      ModelRouter r(cfg);
      check(throwsWith([&] { r.post(Role::Worker, {{"user", "x"}}); }, "pool exhausted"),
            "12.3: ABI-only pool exhausted escalates (F2, never brain)");
      br3.stop();
    }

    // 12.4 Config-time laws (F81/F85): missing factory, missing modelDir,
    // duplicate ABI identity, ABI brain identity leaked into worker pool.
    {
      StubEngine br4, wk4;
      br4.start("glm-5.2-colibri", 18450, "b");
      wk4.start("qwen3.6-colibri", 18460, "w");
      RouterConfig base;
      base.brain = {entry(br4)};
      base.worker = {entry(wk4), abiEntry("/models/olmoe", "olmoe-leaf")};
      base.verifier = {entry(wk4)};
      check(throwsWith([&] { ModelRouter r(base); }, "abiFactory is not set"),
            "12.4/F85: ABI entry without factory rejected at CONFIG time");
      RouterConfig noDir = base;
      noDir.abiFactory = fakeFactory("x", "", 0, 0);
      noDir.worker = {entry(wk4), abiEntry("", "olmoe-leaf")};
      check(throwsWith([&] { ModelRouter r(noDir); }, "empty modelDir"),
            "12.4/F81: ABI entry without modelDir rejected");
      RouterConfig dup = base;
      dup.abiFactory = fakeFactory("x", "", 0, 0);
      dup.worker = {entry(wk4), abiEntry("/models/olmoe", "olmoe-leaf"),
                    abiEntry("/models/olmoe", "olmoe-leaf2")};
      check(throwsWith([&] { ModelRouter r(dup); }, "duplicate endpoint"),
            "12.4/F81: same modelDir twice in one pool rejected");
      RouterConfig leak = base;
      leak.abiFactory = fakeFactory("x", "", 0, 0);
      leak.brain = {abiEntry("/models/olmoe", "olmoe-brain")};
      leak.worker = {entry(wk4), abiEntry("/models/olmoe", "olmoe-leaf")};
      check(throwsWith([&] { ModelRouter r(leak); }, "never a fallback target"),
            "12.4/F2: ABI brain identity in worker pool rejected");
      br4.stop();
      wk4.stop();
    }
    check(built.size() >= 1, "12: factory actually built ABI posters");
  }

  std::cout << (failures == 0 ? "ROUTER PASS\n" : "ROUTER FAIL\n");
  return failures == 0 ? 0 : 1;
}
