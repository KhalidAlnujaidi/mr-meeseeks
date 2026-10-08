// test_abi.cpp — ABI backend suite (F74-F80 mitigations).
//
// Offline section (always, ctest-safe): pure contracts — flattening (F80),
// wall-clock firewall callback semantics, idempotent registration (F74),
// typed error taxonomy, config validation.
//
// Live section (only when ABI_MODEL_DIR is set): opens the REAL engine
// in-process and checks telemetry, F75 preflight, honest usage, firewall
// abort + F77 session recovery. Skipped (printed, exit 0) otherwise so
// `ctest` stays hermetic.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "dshlite/abi_client.hpp"
#include "dshlite/grammar.hpp"

namespace {

int checks = 0, failed = 0;
void expect(bool cond, const std::string& name) {
  ++checks;
  if (!cond) { ++failed; std::cout << "FAIL: " << name << "\n"; }
  else std::cout << "ok: " << name << "\n";
}

void offlineSection() {
  using namespace dshlite;

  // F80 flattening: role: content lines, order preserved.
  {
    const std::vector<Message> msgs = {{"system", "You are terse."},
                                       {"user", "Hi"},
                                       {"assistant", "Hello."},
                                       {"user", "Bye"}};
    const std::string flat = AbiClient::flattenMessages(msgs);
    expect(flat == "system: You are terse.\nuser: Hi\nassistant: Hello.\nuser: Bye",
           "F80 flatten: role-prefixed lines in order");
    expect(AbiClient::flattenMessages({}).empty(), "flatten: empty set => empty string");
  }

  // Wall-clock firewall callback: the EXACT function the ABI polls.
  {
    WallClockFirewall fw;
    fw.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    expect(fw.hit(&fw) == 0, "firewall: before deadline => 0 (continue)");
    expect(!fw.tripped.load(), "firewall: not tripped before deadline");
    fw.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    expect(fw.hit(&fw) == 1, "firewall: past deadline => 1 (abort)");
    expect(fw.tripped.load(), "firewall: tripped flag latches");
    expect(WallClockFirewall::hit(nullptr) == 0, "firewall: null user_data => 0 (defensive)");
  }

  // F74 idempotent registration: repeated + concurrent calls are safe,
  // unknown family throws typed AbiError (not a crash, not a silent no-op).
  {
    AbiClient::ensureAdapters("olmoe");
    AbiClient::ensureAdapters("olmoe");  // second call must NOT throw
    expect(true, "F74: double registration idempotent");
    bool threw = false;
    try { AbiClient::ensureAdapters("no-such-family"); }
    catch (const AbiError&) { threw = true; }
    expect(threw, "F74: unknown family throws AbiError");
    // Concurrent hammering of the registry.
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
      ts.emplace_back([] { AbiClient::ensureAdapters("olmoe"); });
    for (auto& t : ts) t.join();
    expect(true, "F74: 8-thread concurrent registration survives");
  }

  // Config validation: never reaches the ABI with garbage.
  {
    bool threw = false;
    try { AbiClient c(AbiConfig{}); } catch (const AbiError&) { threw = true; }
    expect(threw, "config: empty modelDir throws AbiError");
    AbiConfig c2;
    c2.modelDir = "/nonexistent-dir-xyz";
    c2.maxTokens = 0;
    threw = false;
    try { AbiClient c(c2); } catch (const AbiError&) { threw = true; }
    expect(threw, "config: maxTokens=0 throws AbiError");
  }

  // Error taxonomy: distinct catchable types (router classification).
  {
    bool ctx = false, cancel = false;
    try { throw AbiContextOverflowError(9000, 4096); }
    catch (const AbiContextOverflowError& e) {
      ctx = e.needed == 9000 && e.cap == 4096 &&
            std::string(e.what()).find("context overflow") != std::string::npos;
    }
    try { throw AbiCancelledError("wall-clock firewall tripped"); }
    catch (const AbiCancelledError&) { cancel = true; }
    catch (const AbiError&) { cancel = false; }
    expect(ctx, "F75: AbiContextOverflowError carries needed/cap");
    expect(cancel, "firewall: AbiCancelledError is its own type");
    // Both derive from AbiError => one catch-all still works.
    bool base = false;
    try { throw AbiCancelledError("x"); } catch (const AbiError&) { base = true; }
    expect(base, "taxonomy: AbiCancelledError derives from AbiError");
  }

  // F86 prefix reuse: a checkpoint serves only a strict, exact prefix.
  {
    using V = std::vector<std::int32_t>;
    expect(AbiClient::reusablePrefix({1, 2, 3}, {1, 2, 3, 4, 5}) == 3,
           "F86: strict prefix => reuse checkpoint length");
    expect(AbiClient::reusablePrefix({1, 2, 3}, {1, 2, 3}) == 0,
           "F86: identical prompt => 0 (one token must still run for select)");
    expect(AbiClient::reusablePrefix({1, 2, 3}, {1, 9, 3, 4}) == 0,
           "F86: divergence inside the checkpoint => 0 (full prefill)");
    expect(AbiClient::reusablePrefix({1, 2, 3, 4}, {1, 2}) == 0,
           "F86: checkpoint longer than prompt => 0");
    expect(AbiClient::reusablePrefix(V{}, {1, 2}) == 0, "F86: no checkpoint => 0");
    AbiConfig d;
    expect(d.prefixReuse && d.prefixCacheMaxBytes > 0,
           "F86: prefix reuse on by default with a bounded checkpoint");
  }

  // Router factory (F84): entry mapping — a bogus modelDir cannot be
  // used as a failure probe because the colibri engine open path calls
  // exit() itself on missing config.json (observed). So the mapping is
  // verified structurally here; the real factory open is exercised in
  // the live section against the actual model dir.
  {
    RouterConfig::AbiFactory f = makeAbiBackendFactory();
    expect(static_cast<bool>(f), "F84: makeAbiBackendFactory returns a usable factory");
    expect(deriveFamily("olmoe-leaf") == "olmoe",
           "F84: family derivation maps olmoe-leaf -> olmoe adapter id");
  }
}

void liveSection(const std::string& modelDir) {
  using namespace dshlite;
  std::cout << "\n[live] ABI_MODEL_DIR=" << modelDir << " — opening real engine in-process\n";

  // F84 end-to-end: the router factory builds a real in-process poster
  // from an EngineEntry (deriveFamily -> adapter, memoryLimit wired).
  {
    RouterConfig::AbiFactory f = makeAbiBackendFactory();
    EngineEntry e;
    e.backend = EngineBackend::InProcessAbi;
    e.modelDir = modelDir;
    e.modelId = "olmoe-leaf";
    e.maxTokens = 4;
    e.timeout = std::chrono::seconds(300);
    e.memoryLimitBytes = 8ull * 1024 * 1024 * 1024;
    auto poster = f(e);
    expect(poster != nullptr, "[live] F84: factory built a real AbiClient from EngineEntry");
    auto r = poster->post({{"user", "The capital of France is"}});
    expect(!r.content.empty() && r.usage.completionTokens == 4,
           "[live] F84: factory-built poster decodes in-process");
    expect(poster->totalUsage().completionTokens == 4 && poster->requestCount() == 1,
           "[live] F83: AbiClient usage accounting feeds router aggregation");
  }

  AbiConfig cfg;
  cfg.modelDir = modelDir;
  cfg.maxTokens = 12;
  cfg.timeout = std::chrono::seconds(300);
  AbiClient client(cfg);

  expect(client.vocabSize() == 50304, "[live] caps: vocab 50304 (OLMoE geometry)");
  expect(client.numLayers() == 16, "[live] caps: 16 layers");
  expect(client.stateWidth() == 2048, "[live] caps: width 2048");
  expect(client.maxContextTokens() == 4096, "[live] caps: ctx 4096");
  expect(client.memoryLimitBytes() == cfg.memoryLimitBytes,
         "[live] memory_limit_bytes wired from C++ config");

  // Real in-process decode; honest usage counts.
  auto resp = client.post({{"user", "The capital of France is"}});
  expect(!resp.content.empty(), "[live] decode produced content");
  expect(resp.usage.promptTokens > 0, "[live] usage: real prompt count");
  expect(resp.usage.completionTokens == 12, "[live] usage: completion == maxTokens budget");
  expect(resp.usage.totalTokens == resp.usage.promptTokens + resp.usage.completionTokens,
         "[live] usage: total consistent");
  expect(!resp.usageEstimated, "[live] usage provenance: in-process counts, not estimates");
  expect(resp.ttftMs >= 0, "[live] ttft measured in-process");
  expect(resp.lastByteMs == -1 && resp.maxIdleMs == -1,
         "[live] wire telemetry stays -1 (no wire; never fabricated)");
  expect(resp.latencyMs > 0, "[live] latency measured");
  std::cout << "[live] raw text: " << resp.content << "\n";

  // F75: preflight overflow — huge maxTokens must throw BEFORE session.
  {
    AbiConfig big = cfg;
    big.maxTokens = 100000;
    AbiClient bigClient(big);
    bool overflow = false;
    try { bigClient.post({{"user", "hi"}}); }
    catch (const AbiContextOverflowError&) { overflow = true; }
    expect(overflow, "[live] F75: context overflow refused pre-session, typed");
  }

  // Grammar: typed refusal (F46 taxonomy), engine untouched.
  {
    bool grammar = false;
    ResponseFormat rf;
    rf.type = "json_schema";
    try { client.postConstrained({{"user", "hi"}}, rf); }
    catch (const GrammarUnsupportedError&) { grammar = true; }
    expect(grammar, "[live] postConstrained throws GrammarUnsupportedError");
  }

  // Firewall abort + F77 recovery: 1ms budget mid-decode must trip, and
  // the SAME client must keep working afterwards (session-per-call).
  {
    AbiConfig fast = cfg;
    fast.maxTokens = 12;
    fast.timeout = std::chrono::milliseconds(1);
    AbiClient fastClient(fast);
    bool cancelled = false;
    try { fastClient.post({{"user", "Count slowly from one to ten."}}); }
    catch (const AbiCancelledError&) { cancelled = true; }
    catch (const AbiError&) { cancelled = false; }
    expect(cancelled, "[live] firewall: 1ms budget aborts decode with AbiCancelledError");
    // Recovery on the SAME client that was aborted (F77): restore a sane
    // budget, then decode again — proves the abort destroyed only its
    // per-call session; the long-lived engines stayed clean.
    fastClient.setTimeout(cfg.timeout);
    auto after = fastClient.post({{"user", "The capital of France is"}});
    expect(!after.content.empty() || after.usage.completionTokens > 0,
           "[live] F77: client recovers after cancel (session dropped on abort)");
  }

  // F86: a multi-turn history decodes token-identically with and without
  // prefix reuse, and turn 2 restores turn 1's prompt from the checkpoint.
  {
    AbiConfig off = cfg;
    off.prefixReuse = false;
    AbiClient plain(off);
    AbiClient reusing(cfg);
    std::vector<Message> history = {{"system", "You are terse."},
                                    {"user", "Name a primary color."}};
    auto p1 = plain.post(history);
    auto r1 = reusing.post(history);
    expect(r1.content == p1.content && reusing.lastReusedTokens() == 0,
           "[live] F86: turn 1 identical, nothing reused yet");
    history.push_back({"assistant", r1.content});
    history.push_back({"user", "Name another one."});
    const auto t0 = std::chrono::steady_clock::now();
    auto p2 = plain.post(history);
    const auto t1 = std::chrono::steady_clock::now();
    auto r2 = reusing.post(history);
    const auto t2 = std::chrono::steady_clock::now();
    expect(r2.content == p2.content, "[live] F86: turn 2 token-identical with reuse");
    expect(reusing.lastReusedTokens() > 0 &&
               reusing.lastReusedTokens() < static_cast<std::size_t>(r2.usage.promptTokens),
           "[live] F86: turn 2 restored a prefix from the checkpoint");
    expect(r2.usage.promptTokens == p2.usage.promptTokens,
           "[live] F86: prompt usage still counts every prompt token");
    using ms = std::chrono::milliseconds;
    std::cout << "[live] F86 turn 2: reused " << reusing.lastReusedTokens() << "/"
              << r2.usage.promptTokens << " prompt tokens; plain "
              << std::chrono::duration_cast<ms>(t1 - t0).count() << " ms, reuse "
              << std::chrono::duration_cast<ms>(t2 - t1).count() << " ms\n";
    // History rewrite (compaction): the checkpoint no longer matches.
    auto r3 = reusing.post({{"user", "Unrelated question?"}});
    expect(reusing.lastReusedTokens() == 0 && r3.usage.completionTokens > 0,
           "[live] F86: rewritten history falls back to a full prefill");
  }
}

}  // namespace

int main() {
  offlineSection();
  const char* modelDir = std::getenv("ABI_MODEL_DIR");
  if (modelDir != nullptr && modelDir[0] != '\0') {
    liveSection(modelDir);
  } else {
    std::cout << "\n[live] SKIPPED (ABI_MODEL_DIR unset) — ctest stays hermetic\n";
  }
  std::cout << "\n" << (checks - failed) << "/" << checks << " checks passed\n";
  return failed == 0 ? 0 : 1;
}
