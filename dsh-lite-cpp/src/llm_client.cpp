// llm_client.cpp — Module 3a: Colibri-only OpenAI-compliant HTTP client.
//
// Target: local `coli serve` (default http://127.0.0.1:8000/v1).
// body.model is sent VERBATIM — the engine 404s anything but its
// --model-id. Loopback sends no Authorization header (colibri needs
// none there unless --api-key is set); non-loopback sends Bearer.

#include "dshlite/llm_client.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>

#include <httplib.h>

#include <nlohmann/json.hpp>

namespace dshlite {
namespace {

struct SplitUrl {
  bool https = true;
  std::string host;
  int port = 443;
  std::string path = "/";
};

SplitUrl splitUrl(const std::string& url) {
  SplitUrl out;
  std::string rest;
  if (url.rfind("https://", 0) == 0) {
    out.https = true;
    out.port = 443;
    rest = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    out.https = false;
    out.port = 80;
    rest = url.substr(7);
  } else {
    throw std::runtime_error("llm: endpoint must start with https:// or http://");
  }
  auto slash = rest.find('/');
  std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
  auto colon = authority.rfind(':');
  if (colon != std::string::npos) {
    out.host = authority.substr(0, colon);
    out.port = std::stoi(authority.substr(colon + 1));
  } else {
    out.host = authority;
  }
  if (out.host.empty()) throw std::runtime_error("llm: endpoint has empty host");
  return out;
}

std::string resolveKey(const LlmConfig& cfg) {
  if (!cfg.apiKey.empty()) return cfg.apiKey;
  if (const char* e = std::getenv(cfg.apiKeyEnv.c_str())) return std::string(e);
  return {};
}

bool isLoopback(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

}  // namespace

namespace detail {
bool isLoopbackHost(const std::string& host) { return isLoopback(host); }
}  // namespace detail

LlmClient::LlmClient(LlmConfig cfg) : cfg_(std::move(cfg)) {}

std::future<LlmResponse> LlmClient::postAsync(
    const std::vector<Message>& messages) {
  return std::async(std::launch::async, [this, messages] { return post(messages); });
}

LlmResponse LlmClient::post(const std::vector<Message>& messages) {
  return postImpl(messages, nullptr);
}

LlmResponse LlmClient::postConstrained(const std::vector<Message>& messages,
                                       const ResponseFormat& rf) {
  if (rf.empty()) return postImpl(messages, nullptr);  // text => plain wire
  // F47: validate BEFORE the round-trip (same causes the gateway 400s).
  const nlohmann::json wire = rf.toWire();  // throws std::invalid_argument
  return postImpl(messages, &wire);
}

LlmResponse LlmClient::postImpl(const std::vector<Message>& messages,
                                const nlohmann::json* rfWire) {
  const SplitUrl u = splitUrl(cfg_.endpoint);
  const bool loopback = isLoopback(u.host);
  // Loopback engines need no key; non-loopback binds require one.
  const std::string key = resolveKey(cfg_);
  if (!loopback && key.empty()) {
    throw std::runtime_error("llm: non-loopback endpoint needs an API key (env " +
                             cfg_.apiKeyEnv + " or explicit cfg.apiKey)");
  }
  if (cfg_.model.empty()) {
    throw std::runtime_error("llm: cfg.model must equal the engine --model-id");
  }

  nlohmann::json body;
  body["model"] = cfg_.model;  // verbatim: engine 404s anything else
  body["max_tokens"] = cfg_.maxTokens > 0 ? cfg_.maxTokens : 1024;
  body["messages"] = nlohmann::json::array();
  for (const auto& m : messages)
    body["messages"].push_back({{"role", m.role}, {"content", m.content}});
  if (cfg_.stream) {
    body["stream"] = true;  // SSE: enables ttft measurement
    // F34: ask for the engine-authoritative usage chunk. Colibri's
    // gateway honors this (openai_server.py include_usage); engines
    // that don't just omit it and the delta-count estimate applies.
    if (cfg_.requestUsageInStream)
      body["stream_options"] = {{"include_usage", true}};
  }
  if (rfWire) body["response_format"] = *rfWire;  // G2.4 grammar-forced drafts
  const std::string payload = body.dump();

  httplib::Headers headers;
  if (!key.empty()) headers.emplace("Authorization", "Bearer " + key);
  const auto secs = cfg_.timeout.count() / 1000;
  const auto usecs = (cfg_.timeout.count() % 1000) * 1000;
  // G2.1/F6: on the streaming path with a stall interval configured, the
  // READ timeout becomes the no-bytes liveness interval (httplib re-arms
  // select() on every recv, so dripping bytes keep it alive — wall clock
  // is NOT the stall signal). Connect/write keep the outer watchdog.
  const bool stallArmed = cfg_.stream && cfg_.stallNoBytesMs.count() > 0;
  const auto readMs = stallArmed ? cfg_.stallNoBytesMs.count() : cfg_.timeout.count();
  const auto readSecs = readMs / 1000;
  const auto readUsecs = (readMs % 1000) * 1000;

  // SSE receive state (streaming path, F11): content deltas concatenate;
  // ttft = first delta carrying non-empty content; a `usage` chunk (sent
  // last by OpenAI-compatible engines) fills token totals. A stream with
  // no usage block records 0 tokens honestly (F12) — never invented.
  struct SseState {
    std::string content;
    long ttftMs = -1;
    long promptTokens = 0, completionTokens = 0, totalTokens = 0;
    bool sawUsage = false;
    long contentDeltas = 0;  // F33/F36: non-empty content deltas only
    std::string partial;  // SSE line buffer across receive chunks
    std::chrono::steady_clock::time_point t0;
    // Streaming liveness (G2.1/F6): every received chunk stamps lastByte
    // and updates the largest observed inter-byte silence gap. The stall
    // SIGNAL is the no-bytes interval (read timeout = stallNoBytesMs),
    // never wall clock — these timestamps are the EVIDENCE the router
    // uses to tell slow-but-alive from stalled.
    std::chrono::steady_clock::time_point lastByte;
    bool sawByte = false;
    long lastByteMs = -1;   // last byte, ms into stream (-1 = none)
    long maxIdleMs = -1;    // largest inter-byte gap (-1 = <1 byte)
    long chunks = 0;        // wire chunks received
  };
  SseState sse;
  sse.t0 = std::chrono::steady_clock::now();

  // One SSE `data:` line: parse the delta JSON.
  auto handleSseData = [&sse](const std::string& data) {
    if (data == "[DONE]") return;
    nlohmann::json j;
    try {
      j = nlohmann::json::parse(data);
    } catch (const std::exception&) {
      return;  // malformed keepalive/partial frame: skip, never throw mid-stream
    }
    if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
      const auto& d = j["choices"][0].value("delta", nlohmann::json::object());
      const std::string piece = d.value("content", std::string());
      if (!piece.empty()) {
        if (sse.ttftMs < 0) {
          auto now = std::chrono::steady_clock::now();
          sse.ttftMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - sse.t0).count();
        }
        sse.content += piece;
        // F33/F36: count only NON-EMPTY content deltas — the gateway's
        // keepalive pings (empty deltas, #597) reset byte-liveness but
        // must never inflate the token estimate.
        ++sse.contentDeltas;
      }
    }
    if (j.contains("usage") && j["usage"].is_object() && !j["usage"].is_null()) {
      const auto& w = j["usage"];
      sse.promptTokens = w.value("prompt_tokens", 0L);
      sse.completionTokens = w.value("completion_tokens", 0L);
      sse.totalTokens = w.value("total_tokens", sse.promptTokens + sse.completionTokens);
      sse.sawUsage = true;
    }
  };
  auto handleSseChunk = [&sse, &handleSseData](const char* data, size_t len,
                                               uint64_t /*offset*/,
                                               uint64_t /*total*/) {
    // G2.1/F6 liveness: ANY wire byte (content delta, keepalive, usage)
    // restarts the stall interval and is recorded as evidence.
    auto now = std::chrono::steady_clock::now();
    if (sse.sawByte) {
      const long gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - sse.lastByte)
                           .count();
      if (gap > sse.maxIdleMs) sse.maxIdleMs = gap;
    } else {
      sse.sawByte = true;
      sse.maxIdleMs = 0;
    }
    sse.lastByte = now;
    sse.lastByteMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - sse.t0)
            .count();
    ++sse.chunks;
    sse.partial.append(data, len);
    size_t pos;
    while ((pos = sse.partial.find('\n')) != std::string::npos) {
      std::string line = sse.partial.substr(0, pos);
      sse.partial.erase(0, pos + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("data:", 0) == 0) {
        std::string payloadLine = line.substr(5);
        while (!payloadLine.empty() && payloadLine.front() == ' ') payloadLine.erase(0, 1);
        handleSseData(payloadLine);
      }
    }
    return true;  // keep receiving
  };

  auto t0 = std::chrono::steady_clock::now();
  httplib::Result res;
  if (u.https) {
    httplib::SSLClient cli(u.host, u.port);
    cli.set_connection_timeout(secs, usecs);
    cli.set_read_timeout(readSecs, readUsecs);
    cli.set_write_timeout(secs, usecs);
    if (cfg_.stream) {
      // httplib has no Post(..., receiver): build the Request and send()
      // it with content_receiver attached (SSE lands chunk-by-chunk).
      httplib::Request req;
      req.method = "POST";
      req.path = u.path;
      req.headers = headers;
      req.set_header("Content-Type", "application/json");
      req.body = payload;
      req.content_receiver = handleSseChunk;
      res = cli.send(req);
    } else {
      res = cli.Post(u.path.c_str(), headers, payload, "application/json");
    }
  } else {
    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(secs, usecs);
    cli.set_read_timeout(readSecs, readUsecs);
    cli.set_write_timeout(secs, usecs);
    if (cfg_.stream) {
      httplib::Request req;
      req.method = "POST";
      req.path = u.path;
      req.headers = headers;
      req.set_header("Content-Type", "application/json");
      req.body = payload;
      req.content_receiver = handleSseChunk;
      res = cli.send(req);
    } else {
      res = cli.Post(u.path.c_str(), headers, payload, "application/json");
    }
  }
  auto t1 = std::chrono::steady_clock::now();

  if (!res) {
    // G2.1/F6: with the stall interval armed, a read timeout on the
    // stream means "zero bytes on the wire for stallNoBytesMs" — a
    // STALL, not a generic transport failure. Evidence check (F16):
    // httplib reports Error::Read for both silence-timeouts and early
    // EOF, so the last-byte timestamp decides — only silence at/above
    // ~90% of the cap classifies as stall-no-bytes; an early EOF right
    // after traffic stays a transport failure (honest classification).
    if (stallArmed && res.error() == httplib::Error::Read) {
      const auto now = std::chrono::steady_clock::now();
      const long silentMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - (sse.sawByte ? sse.lastByte : sse.t0))
              .count();
      if (silentMs * 10 >= cfg_.stallNoBytesMs.count() * 9) {
        std::string msg =
            "llm: stall-no-bytes — zero wire bytes for " +
            std::to_string(silentMs) + "ms >= stallNoBytesMs=" +
            std::to_string(cfg_.stallNoBytesMs.count()) + " from " + u.host +
            " (last byte at " + std::to_string(sse.lastByteMs) +
            "ms into stream, " + std::to_string(sse.chunks) +
            " chunks seen) — request aborted cleanly; recovery: router "
              "falls through to the next worker family, never the brain (F2)";
        throw LlmStallError(msg, silentMs, sse.lastByteMs, sse.chunks);
      }
    }
    throw std::runtime_error("llm: transport failure posting to " + u.host +
                             " (httplib error " +
                             std::to_string(static_cast<int>(res.error())) +
                             ") — is `coli serve` running?");
  }
  if (res->status != 200) {
    const std::string hint = (res->status == 404)
                                 ? " (model_not_found: cfg.model != engine --model-id?)"
                                 : "";
    // F46: grammar refusal is its own typed failure — the engine family
    // lacks grammar_payload (verbatim gateway signature observed live on
    // olmoe: code unsupported_parameter, param response_format).
    if (res->status == 400 && rfWire &&
        res->body.find("\"code\":\"unsupported_parameter\"") != std::string::npos &&
        res->body.find("response_format") != std::string::npos) {
      throw GrammarUnsupportedError(
          "llm: grammar-unsupported — engine family lacks grammar_payload "
          "(HTTP 400 unsupported_parameter from " +
          u.host + "): " + res->body.substr(0, 300));
    }
    throw std::runtime_error("llm: HTTP " + std::to_string(res->status) +
                             " from " + u.host + hint + ": " +
                             res->body.substr(0, 500));
  }

  LlmResponse out;
  out.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                      .count();
  if (cfg_.stream) {
    // Flush any trailing unterminated `data:` line.
    if (!sse.partial.empty() && sse.partial.rfind("data:", 0) == 0)
      handleSseData(sse.partial.substr(5));
    out.content = std::move(sse.content);
    out.ttftMs = sse.ttftMs;
    // G2.1/F6 liveness evidence for the router/host: slow-but-alive vs
    // stalled is decidable from these without wall-clock guesswork.
    out.lastByteMs = sse.lastByteMs;
    out.maxIdleMs = sse.maxIdleMs;
    if (sse.sawUsage) {
      out.usage.promptTokens = sse.promptTokens;
      out.usage.completionTokens = sse.completionTokens;
      out.usage.totalTokens = sse.totalTokens;
    } else {
      // F33/F35: engine omitted the usage chunk (no include_usage
      // support). Fall back to a delta-count ESTIMATE for completion
      // tokens — flagged usageEstimated, prompt tokens stay 0 (never
      // guessed). decodeTokPerSec works off it; totals stay honest.
      out.usageEstimated = sse.contentDeltas > 0;
      out.usage.completionTokens = sse.contentDeltas;
      out.usage.totalTokens = sse.contentDeltas;  // prompt unknown => not added
    }
    out.contentDeltas = sse.contentDeltas;
  } else {
    try {
      auto j = nlohmann::json::parse(res->body);
      out.content = j.at("choices").at(0).at("message").at("content").get<std::string>();
      if (j.contains("usage") && j["usage"].is_object()) {
        const auto& w = j["usage"];
        out.usage.promptTokens = w.value("prompt_tokens", 0L);
        out.usage.completionTokens = w.value("completion_tokens", 0L);
        out.usage.totalTokens = w.value("total_tokens",
                                        out.usage.promptTokens + out.usage.completionTokens);
      }
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("llm: malformed JSON response: ") +
                               e.what());
    }
  }

  // Strict token tracking: every successful response accumulates.
  // Mutex-guarded: postAsync futures may complete on any thread.
  {
    std::lock_guard<std::mutex> l(mu_);
    total_.promptTokens += out.usage.promptTokens;
    total_.completionTokens += out.usage.completionTokens;
    total_.totalTokens += out.usage.totalTokens;
    ++requests_;
  }
  return out;
}

double decodeTokPerSec(const LlmResponse& r) {
  if (r.usage.completionTokens <= 0) return 0.0;  // F12: no signal, no rate
  // Decode wall = total latency minus prefill (ttft) when measured (F13).
  // ttft >= latency is a degenerate window => no decode evidence => 0.0.
  long decodeMs = r.latencyMs;
  if (r.ttftMs >= 0) {
    if (r.ttftMs >= r.latencyMs) return 0.0;
    decodeMs = r.latencyMs - r.ttftMs;
  }
  if (decodeMs <= 0) return 0.0;
  return static_cast<double>(r.usage.completionTokens) * 1000.0 /
         static_cast<double>(decodeMs);
}

}  // namespace dshlite
