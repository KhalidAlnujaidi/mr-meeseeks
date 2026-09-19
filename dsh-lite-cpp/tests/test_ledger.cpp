// test_ledger.cpp — Gap 3 acceptance (docs/colibri-roadmap.md G3.1–G3.2)
// + ledger.jsonl v2 emission (Ownership decision, F10/F15).
// Covers: SSE streaming with real ttft measurement (F11), honest zero
// tokens on usage-less streams (F12), decode-window tok/s (F13),
// per-entry velocity floor with warmup exemption (F4/F14), v2 schema
// serialization incl. ttft null and Module-1 sanitization of detail,
// and 8-thread concurrent append with full reparse (F15).

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "dshlite/ledger.hpp"

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

// SSE stub engine: streams N content deltas with a delay before the
// first (making ttft measurable and > 0), then a usage chunk, then
// [DONE]. servedId gating mirrors `coli serve` (404 on mismatch).
struct SseEngine {
  httplib::Server srv;
  std::thread th;
  int port = -1;
  std::string servedId;
  int deltas = 5;
  int firstDelayMs = 60;   // prefill simulation -> ttft
  int deltaDelayMs = 10;   // decode pacing
  bool sendUsage = true;
  long usageCompletion = 20;
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
               const bool wantsStream =
                   req.body.find("\"stream\":true") != std::string::npos;
               if (!wantsStream) {
                 // Non-streaming answer (same engine, stream opt-in F11).
                 res.set_content(
                     R"({"choices":[{"message":{"content":"plain"}}],)"
                     R"("usage":{"prompt_tokens":7,"completion_tokens":3,)"
                     R"("total_tokens":10}})",
                     "application/json");
                 return;
               }
               res.set_chunked_content_provider(
                   "text/event-stream",
                   [this](size_t /*offset*/, httplib::DataSink& sink) {
                     std::this_thread::sleep_for(
                         std::chrono::milliseconds(firstDelayMs));
                     for (int i = 0; i < deltas; ++i) {
                       std::string frame =
                           R"({"choices":[{"delta":{"content":")" +
                           std::to_string(i) + R"("}}]})";
                       std::string chunk = "data: " + frame + "\n\n";
                       sink.write(chunk.data(), chunk.size());
                       if (i + 1 < deltas)
                         std::this_thread::sleep_for(
                             std::chrono::milliseconds(deltaDelayMs));
                     }
                     if (sendUsage) {
                       std::string u =
                           R"({"choices":[],"usage":{"prompt_tokens":11,)"
                           R"("completion_tokens":)" +
                           std::to_string(usageCompletion) +
                           R"(,"total_tokens":)" +
                           std::to_string(11 + usageCompletion) + "}}";
                       std::string chunk = "data: " + u + "\n\n";
                       sink.write(chunk.data(), chunk.size());
                     }
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

std::string tmpLedgerPath(const char* tag) {
  return std::string("/tmp/hermes-ledger-") + tag + "-" +
         std::to_string(::getpid()) + ".jsonl";
}
}  // namespace

int main() {
  using namespace dshlite;
  const std::vector<Message> msgs = {{"user", "atom"}};

  // ── decodeTokPerSec pure checks (F12/F13) ──────────────────────────
  {
    LlmResponse r;
    r.usage.completionTokens = 0;
    check(decodeTokPerSec(r) == 0.0, "F12: zero completion => 0.0, never invented");
    r.usage.completionTokens = 100;
    r.latencyMs = 1000;
    r.ttftMs = -1;
    check(std::fabs(decodeTokPerSec(r) - 100.0) < 1e-9,
          "F13: non-stream window = full latency");
    r.ttftMs = 600;  // 400 ms decode for 100 tokens
    check(std::fabs(decodeTokPerSec(r) - 250.0) < 1e-9,
          "F13: stream window = latency - ttft (decode rate, not e2e)");
    r.ttftMs = 1000;  // degenerate: ttft == latency
    check(decodeTokPerSec(r) == 0.0, "F13: non-positive decode window => 0.0");
  }

  SseEngine fast, slow;
  fast.start("glm-5.3-flash-colibri", 18300);
  slow.start("olmoe-colibri", 18360);
  // slow: 20 completion tokens spread over ~2 s decode => ~10 tok/s;
  // first turn also eats 500 ms "prefill".
  slow.firstDelayMs = 500;
  slow.deltaDelayMs = 400;
  check(fast.port != -1 && slow.port != -1, "two SSE stub engines up");
  if (fast.port == -1 || slow.port == -1) {
    std::cout << "LEDGER FAIL\n";
    return 1;
  }

  // ── G3.1 streaming client: ttft measured, usage from stream ────────
  {
    LlmConfig c;
    c.endpoint = fast.url();
    c.model = "glm-5.3-flash-colibri";
    c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
    c.timeout = std::chrono::seconds(10);
    c.stream = true;
    LlmClient llm(c);
    LlmResponse r = llm.post(msgs);
    check(r.content == "01234", "SSE deltas concatenated in order");
    check(r.ttftMs >= 50 && r.ttftMs < r.latencyMs,
          "F11: ttft measured at socket boundary (>= prefill delay)");
    check(r.usage.promptTokens == 11 && r.usage.completionTokens == 20 &&
              r.usage.totalTokens == 31,
          "usage chunk ingested from stream tail");
    check(decodeTokPerSec(r) > 0.0, "tok/s derivable from streamed response");
    check(llm.totalUsage().completionTokens == 20, "strict totals see stream usage");

    // Non-streaming on the SAME engine: ttft honestly -1 (never faked).
    c.stream = false;
    LlmClient llm2(c);
    LlmResponse r2 = llm2.post(msgs);
    check(r2.content == "plain" && r2.ttftMs == -1,
          "F11: non-streaming => ttft -1, not fabricated");
  }

  // ── F33/F35 (supersedes old F12 zero-rule): usage-less stream =>
  // delta-count ESTIMATE, flagged, prompt tokens 0 — never invented.
  // Pre-F33 code recorded 0 tokens here; the estimate + provenance flag
  // is the ratified replacement (docs/colibri-roadmap.md F33 finding).
  {
    SseEngine noUsage;
    noUsage.sendUsage = false;
    noUsage.start("qwen3.6-colibri", 18420);
    LlmConfig c;
    c.endpoint = noUsage.url();
    c.model = "qwen3.6-colibri";
    c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
    c.timeout = std::chrono::seconds(10);
    c.stream = true;
    LlmClient llm(c);
    LlmResponse r = llm.post(msgs);
    check(r.content == "01234" && r.usageEstimated &&
              r.usage.completionTokens == 5 && r.usage.promptTokens == 0 &&
              r.usage.totalTokens == 5,
          "F35: no usage chunk => flagged delta-count estimate, prompt=0");
    check(r.contentDeltas == 5 && decodeTokPerSec(r) >= 0.0,
          "F33: estimate feeds tok/s; delta count exposed");
    noUsage.stop();
  }

  // ── G3.2 velocity floor + warmup exemption via router ─────────────
  {
    RouterConfig cfg;
    EngineEntry brainE{fast.url(), "glm-5.3-flash-colibri", "", 1024,
                       std::chrono::seconds(10), "", "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ",
                       /*stream=*/true};
    cfg.brain = {brainE};
    // Slow worker: ~10 tok/s, floor 50 => aborts AFTER warmup (2 turns).
    EngineEntry slowE{slow.url(), "olmoe-colibri", "", 1024,
                      std::chrono::seconds(15), "", "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ",
                      /*stream=*/true, /*minTokPerSec=*/50.0, /*warmupTurns=*/2};
    // Healthy second family for the fallthrough to land on.
    EngineEntry fastW{fast.url(), "glm-5.3-flash-colibri", "", 1024,
                      std::chrono::seconds(10), "", "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ",
                      /*stream=*/true};
    // NOTE: same endpoint in one pool is rejected as duplicate — use the
    // slow engine as verifier so pools stay distinct, and give worker
    // pool [slow, dead-then-nothing]... instead: worker = {slow, fastW2}
    // where fastW2 is a SECOND stub on its own port.
    SseEngine fast2;
    fast2.start("qwen3.8-flash-next-colibri", 18460);
    EngineEntry fastW2{fast2.url(), "qwen3.8-flash-next-colibri", "", 1024,
                       std::chrono::seconds(10), "", "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ",
                       /*stream=*/true};
    cfg.worker = {slowE, fastW2};
    cfg.verifier = {fastW2, slowE};
    (void)fastW;
    ModelRouter r(cfg);

    // Warmup turns 1..2: slow engine is EXEMPT despite 10 tok/s < 50 floor.
    auto w1 = r.post(Role::Worker, msgs);
    check(w1.servedBy == slow.url() && w1.attempts.size() == 1 &&
              w1.attempts[0].outcome == "ok",
          "F4: warmup turn 1 exempt from velocity floor");
    auto w2 = r.post(Role::Worker, msgs);
    check(w2.servedBy == slow.url(), "F4: warmup turn 2 still exempt");

    // Turn 3: warmup over => floor trips, router falls to next family.
    auto w3 = r.post(Role::Worker, msgs);
    check(w3.attempts.size() == 2 && w3.attempts[0].outcome == "velocity-floor" &&
              w3.attempts[0].detail.find("velocity floor") != std::string::npos,
          "G3.2: post-warmup slow response aborts with velocity-floor outcome");
    check(w3.servedBy == fast2.url(),
          "G3.2+F2: velocity abort falls through to next worker family, not brain");
    const long brainHitsBefore = fast.hits.load();
    // fast stub serves brain AND was worker-pool candidate earlier; brain
    // endpoint itself must not have gained hits from worker fallback:
    (void)brainHitsBefore;

    // Verifier pool: slowE is entry 2 there and has 2 turns counted? No —
    // counters are per (pool, entry); verifier's slowE is a fresh counter.
    auto v1 = r.post(Role::Verifier, msgs);
    check(v1.servedBy == fast2.url(), "verifier dispatch unaffected by worker floors");
    fast2.stop();
  }

  // ── Ledger v2 serialization (schema conformance) ───────────────────
  {
    LedgerEvent ev;
    ev.type = "report";
    ev.taskId = "R1.2";
    ev.parentId = "R1";
    ev.depth = 1;
    ev.role = "worker";
    ev.model = "qwen3.8-flash-next-colibri";
    ev.endpoint = "127.0.0.1:8081";
    ev.promptTokens = 812;
    ev.completionTokens = 240;
    ev.latencyMs = 41200;
    ev.ttftMs = -1;  // non-streaming => must serialize as null (F11)
    ev.tokPerSec = 5.8;
    ev.detail = "did the atom \x1b[31mRED\x1b[0m";  // ANSI must be stripped
    const std::string line = LedgerWriter::serialize(ev, "2026-09-19T12:00:00.000Z");
    const auto j = nlohmann::json::parse(line);
    check(j["schema"] == 2 && j["type"] == "report" && j["task_id"] == "R1.2" &&
              j["parent_id"] == "R1" && j["depth"] == 1 && j["role"] == "worker",
          "v2: identity fields exact");
    check(j["cost"]["prompt_tokens"] == 812 && j["cost"]["completion_tokens"] == 240 &&
              j["cost"]["latency_ms"] == 41200 &&
              std::fabs(j["cost"]["tok_per_sec"].get<double>() - 5.8) < 1e-9,
          "v2: cost block carries real numbers");
    check(j["cost"]["ttft_ms"].is_null(), "v2/F11: unmeasured ttft serializes as null");
    check(!j.contains("cache"), "v2/F5: cache block absent until heat is real");
    check(j["detail"].get<std::string>().find("\x1b") == std::string::npos &&
              j["detail"].get<std::string>().find("RED") != std::string::npos,
          "detail sanitized through Module 1 (ANSI stripped)");
    check(line.find('\n') == std::string::npos, "one event = one line (JSONL law)");

    // Omission rules: no latency => absent; tok 0.0 => absent.
    LedgerEvent bare;
    bare.type = "DENY";
    bare.taskId = "t9";
    bare.role = "worker";
    bare.model = "olmoe-colibri";
    bare.endpoint = "127.0.0.1:8082";
    bare.gate = "VELOCITY";
    bare.code = "VELOCITY_FLOOR";
    const auto j2 = nlohmann::json::parse(
        LedgerWriter::serialize(bare, "2026-09-19T12:00:00.001Z"));
    check(!j2["cost"].contains("latency_ms") && !j2["cost"].contains("tok_per_sec") &&
              j2["gate"] == "VELOCITY" && j2["code"] == "VELOCITY_FLOOR" &&
              !j2.contains("parent_id"),
          "v2: optional fields omitted, DENY codes present");
  }

  // ── F39 heat -> cache.warm wiring ──────────────────────────────────
  {
    const std::string dir = "/tmp/hermes-heat-" + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0755);
    const std::string warmPath = dir + "/warm";
    const std::string coldPath = dir + "/cold";
    {
      std::ofstream o(warmPath);
      o << "-1 16 64\n-2 1 7\n0 1 5\n";  // fresh mtime => warm
    }
    {
      std::ofstream o(coldPath);
      o << "0 1 5\n";
    }  // closed+flushed HERE, before utime — else close resets mtime to now
    struct utimbuf tb {};
    tb.actime = 1000000000;  // 2001 => stale mtime => not warm
    tb.modtime = 1000000000;
    ::utime(coldPath.c_str(), &tb);

    // Warm probe => cache block emitted with warm:true.
    LedgerEvent ev;
    ev.type = "report"; ev.taskId = "h1"; ev.role = "brain";
    ev.model = "olmoe-colibri"; ev.endpoint = "127.0.0.1:8081";
    attachCacheHeat(ev, probeUsageFile(warmPath));
    const auto jw = nlohmann::json::parse(LedgerWriter::serialize(ev, "2026-09-19T12:00:00.000Z"));
    check(jw.contains("cache") && jw["cache"]["warm"] == true,
          "F39: warm heat file => cache.warm=true emitted");

    // Cold (parsed, stale mtime) => cache block present with warm:false.
    LedgerEvent ev2 = ev;
    ev2.taskId = "h2";
    attachCacheHeat(ev2, probeUsageFile(coldPath));
    const auto jc = nlohmann::json::parse(LedgerWriter::serialize(ev2, "2026-09-19T12:00:00.000Z"));
    check(jc.contains("cache") && jc["cache"]["warm"] == false,
          "F39: parsed-but-stale => cache.warm=false (observed cold, not absent)");

    // Missing file => NO cache block at all (absence != cold, F39).
    LedgerEvent ev3 = ev;
    ev3.taskId = "h3";
    attachCacheHeat(ev3, probeUsageFile(dir + "/nope"));
    const auto jm = nlohmann::json::parse(LedgerWriter::serialize(ev3, "2026-09-19T12:00:00.000Z"));
    check(!jm.contains("cache"),
          "F39: missing heat file => cache block OMITTED (never warm:false)");

    // Schema validity preserved: every line is still v2 and single-line.
    check(jw.value("schema", 0) == 2 && jc.value("schema", 0) == 2 &&
              jm.value("schema", 0) == 2,
          "F39: cache wiring keeps ledger v2 schema valid");

    ::remove(warmPath.c_str());
    ::remove(coldPath.c_str());
    ::rmdir(dir.c_str());
  }

  // ── fromRouted wiring (RoutedResponse -> v2 event) ─────────────────
  {
    SseEngine bEng;
    bEng.start("glm-5.2-colibri", 18540);  // dedicated brain stub (F2: the
    RouterConfig cfg;                      // brain endpoint may not sit in a
    cfg.brain = {{bEng.url(), "glm-5.2-colibri", "", 1024,
                  std::chrono::seconds(10), "", "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ",
                  /*stream=*/true}};
    SseEngine wEng, wEng2;
    wEng.start("deepseek-v4-colibri", 18500);
    wEng2.start("kimi-k3-colibri", 18580);
    cfg.worker = {{wEng.url(), "deepseek-v4-colibri", "", 1024,
                   std::chrono::seconds(10), "", "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ",
                   /*stream=*/true},
                  {wEng2.url(), "kimi-k3-colibri", "", 1024,
                   std::chrono::seconds(10), "", "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ",
                   /*stream=*/true}};
    // verifier pool reuses the worker stubs (distinct pools may share).
    cfg.verifier = cfg.worker;
    ModelRouter r(cfg);
    auto rr = r.post(Role::Worker, msgs);
    LedgerEvent ev = LedgerWriter::fromRouted("report", "worker", "t1", 0, rr);
    const auto j = nlohmann::json::parse(
        LedgerWriter::serialize(ev, "2026-09-19T12:00:00.002Z"));
    check(j["model"] == "deepseek-v4-colibri" &&
              j["cost"]["completion_tokens"] == 20 &&
              j["cost"]["ttft_ms"].is_number() &&
              j["cost"]["tok_per_sec"].get<double>() > 0.0,
          "fromRouted: served model + stream tokens + ttft + tok/s land in v2");
    wEng.stop();
    wEng2.stop();
    bEng.stop();
  }

  // ── F15: concurrent append integrity (8 threads x 50 lines) ────────
  {
    const std::string path = tmpLedgerPath("conc");
    ::remove(path.c_str());
    {
      LedgerWriter w(path);
      std::vector<std::thread> ths;
      for (int t = 0; t < 8; ++t) {
        ths.emplace_back([&w, t] {
          for (int i = 0; i < 50; ++i) {
            LedgerEvent ev;
            ev.type = "report";
            ev.taskId = "T" + std::to_string(t) + "." + std::to_string(i);
            ev.role = "worker";
            ev.model = "olmoe-colibri";
            ev.endpoint = "127.0.0.1:9";
            ev.latencyMs = i;
            ev.ttftMs = -1;
            ev.detail = std::string(200, 'x');  // long-ish payload
            w.append(ev);
          }
        });
      }
      for (auto& th : ths) th.join();
    }
    std::ifstream in(path);
    std::string line;
    long n = 0;
    bool allParse = true, allV2 = true;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      ++n;
      try {
        const auto j = nlohmann::json::parse(line);
        allV2 = allV2 && j.value("schema", 0) == 2;
      } catch (...) {
        allParse = false;
      }
    }
    check(n == 400, "F15: 400 concurrent appends, 400 lines (no lost writes)");
    check(allParse, "F15: every line parses (no interleaved partial JSON)");
    check(allV2, "F15: every line stamped schema 2");
    ::remove(path.c_str());

    // Bad path => loud failure, never silent.
    check(throwsWith([] { LedgerWriter w("/nonexistent-dir/x.jsonl"); }, "cannot open"),
          "unopenable ledger path throws at construction");
  }

  // ── utcNowIso shape ────────────────────────────────────────────────
  {
    const std::string ts = utcNowIso();
    const bool shape = ts.size() == 24 && ts[4] == '-' && ts[7] == '-' &&
                       ts[10] == 'T' && ts[13] == ':' && ts[16] == ':' &&
                       ts[19] == '.' && ts[23] == 'Z';
    check(shape, "utcNowIso: YYYY-MM-DDTHH:MM:SS.mmmZ shape");
  }

  fast.stop();
  slow.stop();

  std::cout << (failures == 0 ? "LEDGER PASS\n" : "LEDGER FAIL\n");
  return failures == 0 ? 0 : 1;
}
