// test_llm.cpp — Milestone 3 acceptance: Colibri POST shape, response
// ingestion, strict token accumulation, async path, error paths.
// Transport is loopback-only via an in-process httplib stub server that
// mimics `coli serve`: no engine, no keys, no TLS (http:// exercises the
// same payload/parse/accounting code as the production path).

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "dshlite/llm_client.hpp"

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

// Observed by the stub server for request-shape assertions.
std::string g_lastBody;
std::string g_lastAuth;
}  // namespace

int main() {
  using namespace dshlite;

  httplib::Server srv;
  // Colibri-faithful stub: 404 unless body.model matches the served id.
  const std::string kServedId = "glm-5.3-flash-colibri";
  srv.Post("/v1/chat/completions",
           [&](const httplib::Request& req, httplib::Response& res) {
             g_lastBody = req.body;
             g_lastAuth = req.get_header_value("Authorization");
             if (req.body.find("\"" + kServedId + "\"") == std::string::npos) {
               res.status = 404;
               res.set_content(R"({"error":{"message":"The model `x` does not exist.",)"
                               R"("type":"model_not_found"}})",
                               "application/json");
               return;
             }
             res.set_content(
                 R"({"choices":[{"message":{"content":"hello brain"}}],)"
                 R"("usage":{"prompt_tokens":10,"completion_tokens":5,)"
                 R"("total_tokens":15}})",
                 "application/json");
           });
  srv.Post("/no_usage", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"choices":[{"message":{"content":"nou"}}]})",
                    "application/json");
  });
  srv.Post("/fail", [](const httplib::Request&, httplib::Response& res) {
    res.status = 500;
    res.set_content(R"({"error":"boom"})", "application/json");
  });
  srv.Post("/badjson", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("not json{{{", "application/json");
  });

  // Bind a free loopback port: try 18080..18100.
  int port = -1;
  for (int p = 18080; p < 18100; ++p) {
    httplib::Server probe;
    if (!probe.bind_to_port("127.0.0.1", p)) continue;  // already in use
    port = p;
    break;
  }
  check(port != -1, "free loopback port found");
  if (port == -1) {
    std::cout << "LLM FAIL\n";
    return 1;
  }

  std::thread srvThread([&srv, port] { srv.listen("127.0.0.1", port); });
  // Wait for the accept loop (max ~5 s).
  bool up = false;
  for (int i = 0; i < 100 && !up; ++i) {
    if (srv.is_running()) up = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  check(up, "stub server listening");
  if (!up) {
    srv.stop();
    srvThread.join();
    std::cout << "LLM FAIL\n";
    return 1;
  }

  auto cfgFor = [&](const std::string& path) {
    LlmConfig c;
    c.endpoint = "http://127.0.0.1:" + std::to_string(port) + path;
    c.apiKey.clear();  // loopback colibri needs no key
    c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
    c.model = kServedId;
    c.timeout = std::chrono::seconds(5);
    return c;
  };
  const std::vector<Message> msgs = {{"system", "you are the brain"},
                                     {"user", "plan"}};

  // 1. Happy path: content + usage parsed, request shape correct.
  {
    LlmClient llm(cfgFor("/v1/chat/completions"));
    LlmResponse r = llm.post(msgs);
    check(r.content == "hello brain", "content ingested");
    check(r.usage.promptTokens == 10 && r.usage.completionTokens == 5 &&
              r.usage.totalTokens == 15,
          "usage block parsed");
    check(g_lastAuth.empty(), "no Authorization on loopback (colibri needs none)");
    check(g_lastBody.find("\"" + kServedId + "\"") != std::string::npos &&
              g_lastBody.find("\"system\"") != std::string::npos &&
              g_lastBody.find("\"user\"") != std::string::npos &&
              g_lastBody.find("\"max_tokens\"") != std::string::npos,
          "colibri payload (verbatim model-id + roles + max_tokens)");
    check(llm.requestCount() == 1, "requestCount == 1");
    const auto t = llm.totalUsage();
    check(t.promptTokens == 10 && t.completionTokens == 5 && t.totalTokens == 15,
          "strict token totals after 1 call");

    // 2. Accumulation across calls.
    LlmResponse r2 = llm.post(msgs);
    (void)r2;
    const auto t2 = llm.totalUsage();
    check(llm.requestCount() == 2 && t2.promptTokens == 20 &&
              t2.completionTokens == 10 && t2.totalTokens == 30,
          "token totals accumulate (strict tracking)");
  }

  // 2b. Wrong model-id => engine 404 model_not_found surfaces.
  {
    LlmConfig c = cfgFor("/v1/chat/completions");
    c.model = "not-the-served-id";
    LlmClient llm(c);
    check(throwsWith([&] { llm.post(msgs); }, "404"),
          "model-id mismatch surfaces 404 (set cfg.model = --model-id)");
  }

  // 3. Missing usage block => zero usage, still counts the request.
  {
    LlmClient llm(cfgFor("/no_usage"));
    LlmResponse r = llm.post(msgs);
    check(r.content == "nou", "content without usage ingested");
    check(r.usage.totalTokens == 0 && llm.requestCount() == 1,
          "zero usage tracked, request counted");
  }

  // 4. Async path: future valid immediately, same result on get().
  {
    LlmClient llm(cfgFor("/v1/chat/completions"));
    auto fut = llm.postAsync(msgs);
    check(fut.valid(), "postAsync returns a live future (non-blocking)");
    LlmResponse r = fut.get();
    check(r.content == "hello brain" && llm.requestCount() == 1,
          "async result + accounting match sync path");
  }

  // 4b. Concurrent async fan-out: N futures share one client, totals
  // stay exact (data-race free) and requestCount == N.
  {
    constexpr int kFanout = 8;
    LlmClient llm(cfgFor("/v1/chat/completions"));
    std::vector<std::future<LlmResponse>> futs;
    for (int i = 0; i < kFanout; ++i) futs.push_back(llm.postAsync(msgs));
    for (auto& f : futs) f.get();
    const auto t = llm.totalUsage();
    check(llm.requestCount() == kFanout &&
              t.promptTokens == 10 * kFanout &&
              t.completionTokens == 5 * kFanout &&
              t.totalTokens == 15 * kFanout,
          "concurrent async totals exact, no lost updates");
  }

  // 5. Loopback needs NO key: empty key + unset env still posts.
  {
    LlmClient llm(cfgFor("/v1/chat/completions"));
    LlmResponse r = llm.post(msgs);
    check(r.content == "hello brain", "loopback posts without any key");
  }

  // 5b. Non-loopback without key => throws before any socket.
  {
    LlmConfig c = cfgFor("/v1/chat/completions");
    c.endpoint = "http://192.0.2.1:8000/v1/chat/completions";  // TEST-NET-1
    LlmClient llm(c);
    check(throwsWith([&] { llm.post(msgs); }, "needs an API key"),
          "non-loopback without key refused");
    check(llm.requestCount() == 0, "refused call not counted");
  }

  // 5c. Empty model => throws (engine would 404; fail fast instead).
  {
    LlmConfig c = cfgFor("/v1/chat/completions");
    c.model.clear();
    LlmClient llm(c);
    check(throwsWith([&] { llm.post(msgs); }, "--model-id"),
          "empty model refused with --model-id hint");
  }

  // 6. Non-200 => throws with status.
  {
    LlmClient llm(cfgFor("/fail"));
    check(throwsWith([&] { llm.post(msgs); }, "HTTP 500"),
          "HTTP 500 throws with status");
  }

  // 7. Malformed JSON => throws.
  {
    LlmClient llm(cfgFor("/badjson"));
    check(throwsWith([&] { llm.post(msgs); }, "malformed"),
          "malformed JSON throws");
  }

  // 8. Bad scheme => throws before any socket.
  {
    LlmConfig c = cfgFor("/v1/chat/completions");
    c.endpoint = "ftp://127.0.0.1/x";
    LlmClient llm(c);
    check(throwsWith([&] { llm.post(msgs); }, "must start with"),
          "bad scheme rejected");
  }

  // 9. Loopback helper: 127.0.0.1/localhost/::1 need no key,
  // real hosts do. Colibri ids are documentation, not a gate —
  // any --model-id the engine serves is accepted verbatim.
  {
    using dshlite::detail::isLoopbackHost;
    check(isLoopbackHost("127.0.0.1") && isLoopbackHost("localhost") &&
              isLoopbackHost("::1") && !isLoopbackHost("example.com") &&
              !isLoopbackHost("192.168.1.10"),
          "loopback helper exact (local no-key, remote keyed)");
    check(!dshlite::kColibriModelIds.empty() &&
              dshlite::kColibriModelIds[0] == "glm-5.3-flash-colibri",
          "colibri id roster present (docs, not a gate)");
  }

  srv.stop();
  srvThread.join();

  std::cout << (failures == 0 ? "LLM PASS\n" : "LLM FAIL\n");
  return failures == 0 ? 0 : 1;
}
