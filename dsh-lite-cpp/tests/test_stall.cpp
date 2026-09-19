// test_stall.cpp — Gap 2 acceptance (docs/colibri-roadmap.md G2.1/F6):
// streaming liveness / stall detection on the SSE receive path.
//
// Stall = ZERO bytes on the wire for stallNoBytesMs — NOT elapsed wall
// clock. The stub engine pattern is copied from tests/test_ledger.cpp
// (SseEngine): in-process httplib servers on 127.0.0.1, colibri-faithful
// 404 on model-id mismatch. Ports 18600+ (house rule: this suite's band).
//
// Regression honesty (standing rule 3): pre-Gap-2 llm_client had no
// stallNoBytesMs and no LlmStallError — a silent engine was only cut at
// cfg.timeout (wall clock) and classified as a generic "transport"
// failure. The checks below assert (a) the typed stall error, (b) abort
// at the no-bytes interval (not the 10 s wall timeout), (c) the router
// outcome vocabulary "stall-no-bytes", and (d) worker->worker
// fallthrough with the brain untouched (F2). Against pre-gate code this
// suite does not compile (missing fields/types), and behaviorally the
// stall checks would hang ~10 s and throw "transport failure".

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "dshlite/llm_client.hpp"
#include "dshlite/router.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

using Clock = std::chrono::steady_clock;
long msSince(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0)
      .count();
}

// SSE stub engine with a silence knob: streams `deltas` content deltas
// (firstDelayMs prefill, deltaDelayMs pacing); when silenceAfterDeltas>0
// it goes SILENT mid-stream for silenceMs (no bytes on the wire at all)
// before finishing. A stallNoBytesMs client must abort during the gap.
struct StallEngine {
  httplib::Server srv;
  std::thread th;
  int port = -1;
  std::string servedId;
  int deltas = 5;
  int firstDelayMs = 30;
  int deltaDelayMs = 10;
  int silenceAfterDeltas = 0;  // 0 = never go silent
  int silenceMs = 1200;        // > any stall cap used in tests
  bool sendUsage = true;
  std::atomic<long> hits{0};

  void start(const std::string& id, int portHint) {
    servedId = id;
    srv.Post("/v1/chat/completions",
             [this](const httplib::Request& req, httplib::Response& res) {
               hits.fetch_add(1);
               if (req.body.find("\"" + servedId + "\"") == std::string::npos) {
                 res.status = 404;
                 res.set_content(R"({"error":{"type":"model_not_found"}})",
                                 "application/json");
                 return;
               }
               res.set_chunked_content_provider(
                   "text/event-stream",
                   [this](size_t /*offset*/, httplib::DataSink& sink) {
                     auto emit = [&sink](const std::string& frame) {
                       std::string chunk = "data: " + frame + "\n\n";
                       sink.write(chunk.data(), chunk.size());
                     };
                     std::this_thread::sleep_for(
                         std::chrono::milliseconds(firstDelayMs));
                     for (int i = 0; i < deltas; ++i) {
                       emit(R"({"choices":[{"delta":{"content":")" +
                            std::to_string(i) + R"("}}]})");
                       if (i + 1 < deltas)
                         std::this_thread::sleep_for(
                             std::chrono::milliseconds(deltaDelayMs));
                       if (silenceAfterDeltas > 0 &&
                           i + 1 == silenceAfterDeltas) {
                         // F6 stall scenario: engine goes silent mid-stream.
                         std::this_thread::sleep_for(
                             std::chrono::milliseconds(silenceMs));
                       }
                     }
                     if (sendUsage)
                       emit(R"({"choices":[],"usage":{"prompt_tokens":9,)"
                            R"("completion_tokens":5,"total_tokens":14}})");
                     sink.write("data: [DONE]\n\n", 14);
                     sink.done();
                     return true;
                   });
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

// Engine that accepts the POST, sends headers, then emits NO body bytes
// at all (stall before first byte: lastByteMs must stay -1).
struct SilentEngine {
  httplib::Server srv;
  std::thread th;
  int port = -1;
  std::string servedId;
  int silenceMs = 1200;

  void start(const std::string& id, int portHint) {
    servedId = id;
    srv.Post("/v1/chat/completions",
             [this](const httplib::Request&, httplib::Response& res) {
               res.set_chunked_content_provider(
                   "text/event-stream",
                   [this](size_t, httplib::DataSink& sink) {
                     std::this_thread::sleep_for(
                         std::chrono::milliseconds(silenceMs));
                     sink.done();
                     return true;
                   });
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

dshlite::LlmConfig streamCfg(const std::string& url, const std::string& model,
                             long stallMs) {
  dshlite::LlmConfig c;
  c.endpoint = url;
  c.model = model;
  c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
  c.timeout = std::chrono::seconds(10);  // outer wall watchdog, deliberately LONG
  c.stream = true;
  c.stallNoBytesMs = std::chrono::milliseconds(stallMs);
  return c;
}
}  // namespace

int main() {
  using namespace dshlite;
  const std::vector<Message> msgs = {{"user", "atom"}};

  // ── Defaults (G2.1): EngineEntry 120000 armed, LlmConfig 0 disabled ──
  {
    EngineEntry e;
    check(e.stallNoBytesMs.count() == 120000,
          "G2.1: EngineEntry default stallNoBytesMs = 120000 (roadmap default)");
    LlmConfig c;
    check(c.stallNoBytesMs.count() == 0,
          "LlmConfig default 0 = disabled (pre-G2.1 behavior preserved)");
    EngineEntry zero;
    zero.stallNoBytesMs = std::chrono::milliseconds(0);
    check(zero.stallNoBytesMs.count() == 0, "0 disables the stall signal");
  }

  // ── Mid-stream stall: abort at the no-bytes interval, typed error ───
  {
    StallEngine eng;
    eng.deltas = 6;
    eng.silenceAfterDeltas = 2;  // 2 deltas, then silence
    eng.silenceMs = 1200;
    eng.start("deepseek-v4-colibri", 18600);
    check(eng.port != -1, "stall stub engine up (ports 18600+)");
    if (eng.port == -1) {
      std::cout << "STALL FAIL\n";
      return 1;
    }
    LlmClient llm(streamCfg(eng.url(), "deepseek-v4-colibri", /*stallMs=*/400));
    auto t0 = Clock::now();
    bool gotStall = false;
    long silentMs = -1, lastByteMs = -2, bytesSeen = -1;
    std::string what;
    try {
      llm.post(msgs);
    } catch (const LlmStallError& e) {
      gotStall = true;
      silentMs = e.silentMs;
      lastByteMs = e.lastByteMs;
      bytesSeen = e.bytesSeen;
      what = e.what();
    } catch (const std::exception& e) {
      what = e.what();
    }
    const long elapsed = msSince(t0);
    check(gotStall,
          "G2.1/F6: mid-stream silence throws typed LlmStallError (pre-gate: "
          "generic transport failure at the 10 s wall timeout)");
    check(what.find("stall-no-bytes") != std::string::npos &&
              what.find("stallNoBytesMs=400") != std::string::npos,
          "stall error names the marker, the cap, and the recovery (fail loud)");
    check(silentMs >= 360 && silentMs < 1200,
          "measured silence >= stallNoBytesMs (no-bytes interval is the signal)");
    check(lastByteMs >= 0 && bytesSeen >= 2,
          "receiver exposes last-byte timestamp + chunk count as evidence");
    check(elapsed >= 350 && elapsed < 2500,
          "aborted at the ~400 ms interval, NOT at the 10 s wall clock (F6)");
    check(llm.requestCount() == 0 && llm.totalUsage().totalTokens == 0,
          "aborted stall never counted in strict token totals");
    eng.stop();
  }

  // ── Slow-but-alive: dripping bytes beyond the stall cap is NOT a stall ──
  {
    StallEngine drip;
    drip.deltas = 12;
    drip.deltaDelayMs = 100;  // one delta per 100 ms, gap < 400 ms cap
    drip.start("olmoe-colibri", 18640);
    check(drip.port != -1, "drip stub engine up");
    if (drip.port == -1) {
      std::cout << "STALL FAIL\n";
      return 1;
    }
    LlmClient llm(streamCfg(drip.url(), "olmoe-colibri", /*stallMs=*/400));
    LlmResponse r = llm.post(msgs);  // total wall ~1.2 s > 400 ms cap
    check(r.content == "01234567891011",
          "F6: slow-but-alive stream (wall > stall cap) completes — wall clock "
          "is NOT the stall signal");
    check(r.lastByteMs > 0 && r.maxIdleMs >= 80 && r.maxIdleMs < 400,
          "liveness evidence: lastByteMs + maxIdleMs exposed on the response");
    check(r.ttftMs >= 0, "ttft still measured on the armed-stall path");
    drip.stop();
  }

  // ── Stall before the first byte (headers-only silence) ──────────────
  {
    SilentEngine dead;
    dead.start("kimi-k3-colibri", 18680);
    check(dead.port != -1, "silent stub engine up");
    if (dead.port == -1) {
      std::cout << "STALL FAIL\n";
      return 1;
    }
    LlmClient llm(streamCfg(dead.url(), "kimi-k3-colibri", /*stallMs=*/400));
    bool gotStall = false;
    long lastByteMs = -2;
    try {
      llm.post(msgs);
    } catch (const LlmStallError& e) {
      gotStall = true;
      lastByteMs = e.lastByteMs;
    } catch (...) {
    }
    check(gotStall && lastByteMs == -1,
          "stall before first byte: LlmStallError with lastByteMs == -1 "
          "(zero bytes EVER on the wire)");
    dead.stop();
  }

  // ── Router: stall classified "stall-no-bytes", worker->worker, F2 ───
  {
    StallEngine stallW, healthyW, brainE;
    stallW.deltas = 4;
    stallW.silenceAfterDeltas = 1;
    stallW.start("qwen3.8-flash-next-colibri", 18720);
    healthyW.start("olmoe-colibri", 18760);
    brainE.start("glm-5.2-colibri", 18800);
    check(stallW.port != -1 && healthyW.port != -1 && brainE.port != -1,
          "router stall scenario: 3 stub engines up");
    if (stallW.port == -1 || healthyW.port == -1 || brainE.port == -1) {
      std::cout << "STALL FAIL\n";
      return 1;
    }

    RouterConfig cfg;
    cfg.brain = {{brainE.url(), "glm-5.2-colibri"}};
    EngineEntry stallEntry;
    stallEntry.endpoint = stallW.url();
    stallEntry.modelId = "qwen3.8-flash-next-colibri";
    stallEntry.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
    stallEntry.timeout = std::chrono::seconds(10);
    stallEntry.stream = true;
    stallEntry.stallNoBytesMs = std::chrono::milliseconds(400);
    EngineEntry healthyEntry = stallEntry;
    healthyEntry.endpoint = healthyW.url();
    healthyEntry.modelId = "olmoe-colibri";
    cfg.worker = {stallEntry, healthyEntry};
    cfg.verifier = {healthyEntry, stallEntry};
    ModelRouter router(cfg);

    const long brainHitsBefore = brainE.hits.load();
    auto rr = router.post(Role::Worker, msgs);
    check(rr.attempts.size() == 2 && rr.attempts[0].outcome == "stall-no-bytes",
          "G2.1: RouteAttempt outcome vocabulary extended with \"stall-no-bytes\" "
          "(pre-gate classify() returned \"transport\" for any read timeout)");
    check(rr.attempts[0].detail.find("stall-no-bytes") != std::string::npos &&
              rr.attempts[0].endpoint == stallW.url(),
          "attempt detail carries the stall evidence, endpoint pinned");
    check(rr.servedBy == healthyW.url() && rr.modelId == "olmoe-colibri",
          "F2: stall falls through worker->worker (next family), request succeeds");
    check(brainE.hits.load() == brainHitsBefore,
          "F2 law: the brain engine was NEVER touched by stall fallback");
    check(rr.response.content == "01234", "healthy worker answered fully");

    // Exhausted pool still escalates with the stall outcome visible.
    RouterConfig only;
    only.brain = {{brainE.url(), "glm-5.2-colibri"}};
    only.worker = {stallEntry};
    only.verifier = {stallEntry};
    // stallEntry endpoint already used in cfg.worker? Distinct RouterConfig,
    // distinct pool — duplicate check is per-pool/per-router, fine.
    ModelRouter r2(only);
    bool escalated = false;
    try {
      r2.post(Role::Worker, msgs);
    } catch (const std::exception& e) {
      escalated = std::string(e.what()).find("pool exhausted") != std::string::npos &&
                  std::string(e.what()).find("stall-no-bytes") != std::string::npos;
    }
    check(escalated,
          "exhausted worker pool escalates, last error names stall-no-bytes");
    stallW.stop();
    healthyW.stop();
    brainE.stop();
  }

  std::cout << (failures == 0 ? "STALL PASS\n" : "STALL FAIL\n");
  return failures == 0 ? 0 : 1;
}
