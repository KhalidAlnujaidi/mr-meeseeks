// test_llm.cpp — Milestone 3 acceptance: OpenAI-compliant POST shape,
// response ingestion, strict token accumulation, async path, error paths.
// Transport is loopback-only via an in-process httplib stub server:
// no network, no keys, no TLS certs (http:// exercises the same
// payload/parse/accounting code as the https:// production path).

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <set>
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
  srv.Post("/ok", [](const httplib::Request& req, httplib::Response& res) {
    g_lastBody = req.body;
    g_lastAuth = req.get_header_value("Authorization");
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
    // Probe by attempting a throwaway bind via the server itself is racy;
    // instead just try listen in a thread and check is_running. Simpler:
    // attempt sequential listen with invalid=false trick: httplib has no
    // try-bind, so use a raw socket probe.
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
    c.apiKey = "test-key";
    c.model = "unit-test-model";
    c.timeout = std::chrono::seconds(5);
    return c;
  };
  const std::vector<Message> msgs = {{"system", "you are the brain"},
                                     {"user", "plan"}};

  // 1. Happy path: content + usage parsed, request shape correct.
  {
    LlmClient llm(cfgFor("/ok"));
    LlmResponse r = llm.post(msgs);
    check(r.content == "hello brain", "content ingested");
    check(r.usage.promptTokens == 10 && r.usage.completionTokens == 5 &&
              r.usage.totalTokens == 15,
          "usage block parsed");
    check(g_lastAuth == "Bearer test-key", "Authorization Bearer sent");
    check(g_lastBody.find("\"unit-test-model\"") != std::string::npos &&
              g_lastBody.find("\"system\"") != std::string::npos &&
              g_lastBody.find("\"user\"") != std::string::npos &&
              g_lastBody.find("\"max_tokens\"") != std::string::npos,
          "OpenAI-compliant payload (model + roles + max_tokens)");
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
    LlmClient llm(cfgFor("/ok"));
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
    LlmClient llm(cfgFor("/ok"));
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

  // 5. Missing key => throws, no request counted.
  {
    LlmConfig c = cfgFor("/ok");
    c.apiKey.clear();
    c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
    LlmClient llm(c);
    check(throwsWith([&] { llm.post(msgs); }, "missing API key"),
          "missing key throws");
    check(llm.requestCount() == 0, "failed call not counted");
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
    LlmConfig c = cfgFor("/ok");
    c.endpoint = "ftp://127.0.0.1/x";
    LlmClient llm(c);
    check(throwsWith([&] { llm.post(msgs); }, "must start with"),
          "bad scheme rejected");
  }

  // 9. Free-only gate on REAL hosts (via detail::resolveModelForHost):
  // "auto" shuffles within the verified free pool, explicit free ids
  // pass, paid ids fall back to free (never spend). Loopback keeps
  // synthetic names (proven by block 1 above).
  {
    using dshlite::detail::resolveModelForHost;
    dshlite::LlmConfig c;
    c.apiKey = "x";
    c.model = "auto";
    std::set<std::string> seen;
    for (int i = 0; i < 20; ++i)
      seen.insert(resolveModelForHost(c, "openrouter.ai"));
    bool allFree = !seen.empty();
    for (const auto& m : seen)
      if (m.size() < 5 || m.compare(m.size() - 5, 5, ":free") != 0)
        allFree = false;
    check(allFree && seen.size() > 1, "auto shuffles across free pool");
    c.model = "nex-agi/nex-n2.5-pro:free";
    check(resolveModelForHost(c, "openrouter.ai") ==
              "nex-agi/nex-n2.5-pro:free",
          "explicit free id passes");
    c.model = "openai/gpt-5-paid";
    std::string fb = resolveModelForHost(c, "openrouter.ai");
    check(fb.size() >= 5 && fb.compare(fb.size() - 5, 5, ":free") == 0,
          "paid id rejected -> free fallback (never spend)");
  }

  srv.stop();
  srvThread.join();

  std::cout << (failures == 0 ? "LLM PASS\n" : "LLM FAIL\n");
  return failures == 0 ? 0 : 1;
}
