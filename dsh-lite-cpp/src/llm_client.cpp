// llm_client.cpp — Module 3a: OpenAI-compliant HTTPS client.

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

}  // namespace

LlmClient::LlmClient(LlmConfig cfg) : cfg_(std::move(cfg)) {}

std::future<LlmResponse> LlmClient::postAsync(
    const std::vector<Message>& messages) {
  return std::async(std::launch::async, [this, messages] { return post(messages); });
}

LlmResponse LlmClient::post(const std::vector<Message>& messages) {
  const std::string key = resolveKey(cfg_);
  if (key.empty()) {
    throw std::runtime_error("llm: missing API key (env " + cfg_.apiKeyEnv +
                             " unset and no explicit key configured)");
  }
  const SplitUrl u = splitUrl(cfg_.endpoint);

  nlohmann::json body;
  body["model"] = cfg_.model;
  body["messages"] = nlohmann::json::array();
  for (const auto& m : messages)
    body["messages"].push_back({{"role", m.role}, {"content", m.content}});
  const std::string payload = body.dump();

  httplib::Headers headers = {{"Authorization", "Bearer " + key}};
  const auto secs = cfg_.timeout.count() / 1000;
  const auto usecs = (cfg_.timeout.count() % 1000) * 1000;

  auto t0 = std::chrono::steady_clock::now();
  httplib::Result res;
  if (u.https) {
    httplib::SSLClient cli(u.host, u.port);
    cli.set_connection_timeout(secs, usecs);
    cli.set_read_timeout(secs, usecs);
    cli.set_write_timeout(secs, usecs);
    res = cli.Post(u.path.c_str(), headers, payload, "application/json");
  } else {
    httplib::Client cli(u.host, u.port);
    cli.set_connection_timeout(secs, usecs);
    cli.set_read_timeout(secs, usecs);
    cli.set_write_timeout(secs, usecs);
    res = cli.Post(u.path.c_str(), headers, payload, "application/json");
  }
  auto t1 = std::chrono::steady_clock::now();

  if (!res) {
    throw std::runtime_error("llm: transport failure posting to " + u.host +
                             " (httplib error " +
                             std::to_string(static_cast<int>(res.error())) + ")");
  }
  if (res->status != 200) {
    throw std::runtime_error("llm: HTTP " + std::to_string(res->status) +
                             " from " + u.host + ": " +
                             res->body.substr(0, 500));
  }

  LlmResponse out;
  out.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                      .count();
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

}  // namespace dshlite
