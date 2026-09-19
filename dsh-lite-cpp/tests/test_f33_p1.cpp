// test_f33_p1.cpp — F33 stream token counting + P1 .coli_usage probe.
//
// F33 (G3.1): stream_options.include_usage requested on the wire; an
// engine that answers with a usage chunk gives ENGINE-authoritative
// totals (usageEstimated=false); an engine that omits it gets the
// delta-count ESTIMATE (usageEstimated=true, promptTokens=0 — never
// fabricated); keepalive empty deltas never inflate the estimate (F36).
// P1 (G3.3/F5/F37/F38): .coli_usage parsing (v1 headers, legacy
// headerless, IKU1 magic refusal, malformed tolerance, missing file),
// mtime-based warmth, and a concurrent rename-during-read race stress
// proving the reader never throws, never blocks, never parses garbage.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <httplib.h>

#include "dshlite/llm_client.hpp"
#include "dshlite/usage_probe.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

std::string g_lastBody;  // request-shape observation

// SSE stub: streams `deltas` content deltas + `keepalives` EMPTY deltas
// (interleaved first, like a cold prefill), then optionally the usage
// chunk (only when the request carried stream_options.include_usage —
// exactly colibri openai_server.py's contract), then [DONE].
struct UsageStub {
  httplib::Server srv;
  std::thread th;
  int port = -1;
  std::string servedId = "olmoe-colibri";
  int deltas = 5;
  int keepalives = 3;
  bool honorIncludeUsage = true;  // false = engine that ignores the option
  long usageCompletion = 42;

  void start(int portHint) {
    srv.Post("/v1/chat/completions",
             [this](const httplib::Request& req, httplib::Response& res) {
               g_lastBody = req.body;
               if (req.body.find("\"" + servedId + "\"") == std::string::npos) {
                 res.status = 404;
                 res.set_content("{}", "application/json");
                 return;
               }
               const bool wantsUsage =
                   honorIncludeUsage &&
                   req.body.find("\"include_usage\":true") != std::string::npos;
               res.set_chunked_content_provider(
                   "text/event-stream",
                   [this, wantsUsage](size_t, httplib::DataSink& sink) {
                     auto send = [&sink](const std::string& frame) {
                       std::string chunk = "data: " + frame + "\n\n";
                       sink.write(chunk.data(), chunk.size());
                     };
                     // Cold-prefill keepalives FIRST (empty deltas, #597).
                     for (int i = 0; i < keepalives; ++i)
                       send(R"({"choices":[{"delta":{"content":""}}]})");
                     for (int i = 0; i < deltas; ++i) {
                       // Pace the deltas: an instant stream leaves no
                       // decode window (ttft == latency => F13 returns 0
                       // by design). 20 ms/token makes tok/s measurable.
                       if (i + 1 < deltas)
                         std::this_thread::sleep_for(std::chrono::milliseconds(20));
                       send(R"({"choices":[{"delta":{"content":"tok"}}]})");
                     }
                     if (wantsUsage)
                       send(R"({"choices":[],"usage":{"prompt_tokens":13,)"
                            R"("completion_tokens":)" +
                            std::to_string(usageCompletion) +
                            R"(,"total_tokens":)" +
                            std::to_string(13 + usageCompletion) + "}}");
                     send("[DONE]");
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

dshlite::LlmConfig streamCfg(const UsageStub& s, bool reqUsage = true) {
  dshlite::LlmConfig c;
  c.endpoint = s.url();
  c.model = s.servedId;
  c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
  c.timeout = std::chrono::seconds(10);
  c.stream = true;
  c.requestUsageInStream = reqUsage;
  return c;
}

void writeFile(const std::string& path, const std::string& body) {
  std::ofstream o(path, std::ios::binary);
  o << body;
}
}  // namespace

int main() {
  using namespace dshlite;
  const std::vector<Message> msgs = {{"user", "hi"}};

  // ── F33/F34: wire shape — include_usage requested by default ───────
  UsageStub eng;
  eng.start(18700);
  check(eng.port != -1, "usage stub engine up");
  if (eng.port == -1) { std::cout << "F33P1 FAIL\n"; return 1; }
  {
    LlmClient llm(streamCfg(eng));
    LlmResponse r = llm.post(msgs);
    check(g_lastBody.find("\"stream_options\":{\"include_usage\":true}") !=
              std::string::npos,
          "F34: stream_options.include_usage on the wire (colibri honors it)");
    // Engine-authoritative usage: 42 completion (NOT the 5 delta count),
    // prompt 13, estimated flag false.
    check(r.usage.completionTokens == 42 && r.usage.promptTokens == 13 &&
              r.usage.totalTokens == 55 && !r.usageEstimated,
          "F34: engine usage chunk wins — authoritative totals, not estimated");
    check(r.contentDeltas == 5,
          "F36: contentDeltas counts non-empty deltas only (3 keepalives excluded)");
    check(r.content == "toktoktoktoktok", "content intact");
    check(decodeTokPerSec(r) > 0.0, "tok/s computes off engine usage");
  }
  // ── F33/F35: engine IGNORES include_usage => honest estimate ───────
  {
    UsageStub noUsage;
    noUsage.honorIncludeUsage = false;
    noUsage.start(18750);
    LlmClient llm(streamCfg(noUsage));
    LlmResponse r = llm.post(msgs);
    check(r.usageEstimated && r.usage.completionTokens == 5 &&
              r.usage.promptTokens == 0 && r.usage.totalTokens == 5,
          "F35: no usage chunk => delta-count estimate, flagged, prompt=0 never guessed");
    check(decodeTokPerSec(r) > 0.0,
          "F33: tok/s now computable off the estimate (was 0 pre-F33)");
    noUsage.stop();
  }
  // ── requestUsageInStream=false: no stream_options on the wire ─────
  {
    LlmClient llm(streamCfg(eng, /*reqUsage=*/false));
    LlmResponse r = llm.post(msgs);
    check(g_lastBody.find("stream_options") == std::string::npos,
          "opt-out: requestUsageInStream=false omits stream_options");
    check(r.usageEstimated && r.usage.completionTokens == 5,
          "opt-out path falls back to the estimate (stub requires the option)");
  }
  // ── Defaults untouched for the non-streaming path ─────────────────
  {
    // (Non-streaming usage parsing is covered by test-llm's existing
    // suite; here we only pin the new fields' defaults.)
    LlmResponse fresh;
    check(!fresh.usageEstimated && fresh.contentDeltas == 0,
          "defaults: usageEstimated=false, contentDeltas=0 (non-stream untouched)");
  }
  eng.stop();

  // ── P1: .coli_usage parsing ────────────────────────────────────────
  const std::string dir = "/tmp/hermes-p1-" + std::to_string(::getpid());
  ::mkdir(dir.c_str(), 0755);
  {
    // v1 format: headers + sparse triples (route_trace.h).
    const std::string p = dir + "/v1";
    writeFile(p, "-1 16 64\n-2 1 " + std::to_string(coliEngineHash("olmoe")) +
                     "\n0 3 120\n0 7 45\n15 63 9\n");
    UsageProbe pr = probeUsageFile(p);
    check(pr.ok && pr.nLayers == 16 && pr.nExperts == 64 &&
              pr.formatVersion == 1 && pr.records == 3 &&
              pr.totalHits == 174,
          "P1: v1 headers + sparse triples parsed exactly");
    check(pr.engineIdHash == coliEngineHash("olmoe"),
          "P1: engine hash round-trips through coliEngineHash (FNV-1a)");
    check(pr.warm, "P1/F38: fresh mtime => warm=true");
    check(pr.error.empty(), "P1: clean parse has no error note");
  }
  {
    // Legacy headerless variant.
    const std::string p = dir + "/legacy";
    writeFile(p, "0 1 5\n2 3 7\n");
    UsageProbe pr = probeUsageFile(p);
    check(pr.ok && pr.nLayers == -1 && pr.formatVersion == -1 &&
              pr.records == 2 && pr.totalHits == 12,
          "P1: legacy headerless triples accepted (dims -1 = absent)");
  }
  {
    // IKU1 binary magic => refused by name, not parsed as text.
    const std::string p = dir + "/iku1";
    const char magic[4] = {'I', 'K', 'U', '1'};
    std::ofstream o(p, std::ios::binary);
    o.write(magic, 4);
    o << "junkjunkjunk";
    o.close();
    UsageProbe pr = probeUsageFile(p);
    check(!pr.ok && pr.error.find("IKU1") != std::string::npos,
          "P1: IKU1 binary history refused by magic (matches engine rule)");
  }
  {
    // Malformed line: stop, keep prior records, note the error.
    const std::string p = dir + "/torn";
    writeFile(p, "-1 4 8\n0 1 2\nNOT_A_RECORD xyz\n1 2 3\n");
    UsageProbe pr = probeUsageFile(p);
    check(pr.ok && pr.records == 1 &&
              pr.error.find("malformed") != std::string::npos,
          "P1: malformed line => stop-with-note, prior records kept (never throws)");
  }
  {
    UsageProbe pr = probeUsageFile(dir + "/does-not-exist");
    check(!pr.ok && !pr.exists && pr.error.find("no such file") != std::string::npos,
          "P1: missing file => ok=false with an honest note (engine not run yet)");
  }
  {
    // F38 stale mtime => not warm (cumulative file, idle engine).
    const std::string p = dir + "/stale";
    writeFile(p, "0 1 5\n");
    struct utimbuf tb {};
    tb.actime = 1000000000;   // 2001-09-09
    tb.modtime = 1000000000;
    ::utime(p.c_str(), &tb);
    UsageProbe pr = probeUsageFile(p);
    check(pr.ok && !pr.warm, "P1/F38: stale mtime => warm=false (idle engine)");
  }

  // ── F37 race stress: writer renames new versions while readers probe.
  // The reader must never throw, never block, and only ever report
  // fully-formed snapshots (every ok parse has consistent totals).
  {
    const std::string target = dir + "/live";
    std::atomic<bool> stop{false};
    std::atomic<long> writes{0}, reads{0}, okReads{0}, badReads{0};
    std::thread writer([&] {
      long v = 0;
      while (!stop.load()) {
        const std::string tmp = target + ".tmp" + std::to_string(++v);
        {
          std::ofstream o(tmp);
          o << "-1 16 64\n-2 1 7\n";
          for (int i = 0; i < 200; ++i) o << i % 16 << " " << i % 64 << " " << v << "\n";
        }
        ::rename(tmp.c_str(), target.c_str());  // atomic publish (F37)
        writes.fetch_add(1);
      }
    });
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
      readers.emplace_back([&] {
        while (!stop.load()) {
          reads.fetch_add(1);
          try {
            UsageProbe pr = probeUsageFile(target, /*freshWithinMs=*/120000);
            if (pr.ok) {
              okReads.fetch_add(1);
              // Consistency law: a torn read would show records != 200
              // or mismatched dims. The rename protocol must prevent it.
              if (pr.records != 200 || pr.nLayers != 16) badReads.fetch_add(1);
            }
          } catch (...) {
            badReads.fetch_add(1);  // probe NEVER throws — a throw is a defect
          }
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true);
    writer.join();
    for (auto& th : readers) th.join();
    check(badReads.load() == 0,
          "F37: 2s rename-during-read stress — zero torn parses, zero throws");
    check(writes.load() > 0 && okReads.load() > 0,
          "F37: stress actually exercised concurrent write+read");
    std::cout << "       (writes=" << writes.load() << " reads=" << reads.load()
              << " okReads=" << okReads.load() << ")\n";
  }

  ::remove((dir + "/v1").c_str());
  ::remove((dir + "/legacy").c_str());
  ::remove((dir + "/iku1").c_str());
  ::remove((dir + "/torn").c_str());
  ::remove((dir + "/stale").c_str());
  ::remove((dir + "/live").c_str());
  ::rmdir(dir.c_str());

  std::cout << (failures == 0 ? "F33P1 PASS\n" : "F33P1 FAIL\n");
  return failures == 0 ? 0 : 1;
}
