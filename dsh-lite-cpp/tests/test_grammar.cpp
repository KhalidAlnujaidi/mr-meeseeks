// test_grammar.cpp — G2.4 acceptance: grammar-forced tool payload
// drafts (roadmap G2.4; flaws F45-F50 in grammar.hpp).
//
// Coverage map:
//  1. ResponseFormat wire construction — exact body shapes for all four
//     types (text omitted, json_object, json_schema wrap, gbnf raw).
//  2. F47 config-time validation — every gateway 400 cause rejected
//     LOCALLY before a round-trip (type, schema shape, 1 MiB, NUL,
//     missing root rule, empty tools).
//  3. toolPayloadFormat — enum span from registered tools; single-tool
//     args schema inlined with strict-full required (F49); multi-tool
//     leaves args unconstrained (F50: no anyOf in the compiler).
//  4. toolPayloadGbnf — raw GBNF with root rule, quoted names, generic
//     JSON body (whitespace-tolerant like GENERIC_JSON_GBNF).
//  5. Payload parameter passing — a stub engine captures the request
//     body: response_format arrives verbatim on postConstrained and is
//     ABSENT on plain post (pre-G2.4 wire stays byte-identical).
//  6. F46 capability — familySupportsGrammar table; stub engine
//     answering the LIVE olmoe 400 (verbatim body captured from coli
//     serve) => GrammarUnsupportedError => router attempt outcome
//     "grammar-unsupported" + fallthrough to a capable family; all-
//     refusing pool => exhausted throw CITES the grammar (fail-loud).
//  7. F45 strict parse — prose, fenced JSON, trailing junk, empty all
//     throw PayloadFormatError; clean payload (with surrounding
//     whitespace) parses. No repair, no substring extraction.
//  8. solicitToolPayload composition — ok path hands the gate
//     pre-validated JSON (checkPayload sees structure); prose reply =>
//     ok=false + nudge ledger line; GrammarUnsupportedError poster =>
//     silent fallback to unconstrained post (solicitation never
//     impossible, F46); F45 law: a grammar-CONSTRAINED request whose
//     engine still returns prose is REFUSED by the gate path — the
//     grammar is an accelerator, never the enforcement.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/grammar.hpp"
#include "dshlite/ledger.hpp"
#include "dshlite/llm_client.hpp"
#include "dshlite/payload_gate.hpp"
#include "dshlite/router.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}

// Verbatim 400 body captured live from coli serve (olmoe, F46):
const char* kOlmoeGrammar400 =
    R"({"error":{"message":"`response_format` grammars are not supported by the olmoe engine yet.","type":"invalid_request_error","param":"response_format","code":"unsupported_parameter"}})";

// Stub engine: records the last request body; answers either a normal
// completion or the verbatim grammar 400.
struct GrammarStub {
  httplib::Server srv;
  std::thread th;
  int port = -1;
  std::string servedId;
  std::string lastBody;
  bool refuseGrammar = false;  // true = family lacks grammar_payload

  void start(const std::string& id, int portHint) {
    servedId = id;
    srv.Post("/v1/chat/completions",
             [this](const httplib::Request& req, httplib::Response& res) {
               lastBody = req.body;
               if (req.body.find("\"" + servedId + "\"") == std::string::npos) {
                 res.status = 404;
                 res.set_content("{}", "application/json");
                 return;
               }
               const bool hasGrammar =
                   req.body.find("response_format") != std::string::npos;
               if (hasGrammar && refuseGrammar) {
                 res.status = 400;
                 res.set_content(kOlmoeGrammar400, "application/json");
                 return;
               }
               res.set_content(
                   R"({"choices":[{"message":{"role":"assistant",)"
                   R"("content":"{\"tool\":\"shell\",\"args\":{\"cmd\":\"true\"}}"}}],)"
                   R"("usage":{"prompt_tokens":9,"completion_tokens":7,)"
                   R"("total_tokens":16}})",
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

dshlite::LlmConfig stubCfg(const GrammarStub& s, bool refuseIgnored = false) {
  (void)refuseIgnored;
  dshlite::LlmConfig c;
  c.endpoint = s.url();
  c.model = s.servedId;
  c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
  c.timeout = std::chrono::seconds(10);
  return c;
}

// Fake poster for solicit composition tests (F48 seam).
struct FakePoster : dshlite::ILlmPoster {
  std::string reply;
  bool throwGrammarUnsupported = false;
  bool sawConstrained = false;
  bool sawPlainAfterRefusal = false;
  dshlite::ResponseFormat lastRf;
  dshlite::LlmResponse post(const std::vector<dshlite::Message>&) override {
    if (sawConstrained && throwGrammarUnsupported) sawPlainAfterRefusal = true;
    return dshlite::LlmResponse{reply, {}, 1};
  }
  dshlite::LlmResponse postConstrained(const std::vector<dshlite::Message>& m,
                                       const dshlite::ResponseFormat& rf) override {
    sawConstrained = true;
    lastRf = rf;
    if (throwGrammarUnsupported)
      throw dshlite::GrammarUnsupportedError(
          "llm: grammar-unsupported — engine family lacks grammar_payload");
    return post(m);
  }
};

dshlite::JudgeHook passJudge() {
  return [](const std::string&) -> dshlite::JudgeVerdict {
    dshlite::JudgeVerdict v;
    v.action = dshlite::JudgeAction::DoDirect;
    v.route = "do_direct";
    v.confidence = 0.9;
    return v;
  };
}
}  // namespace

int main() {
  using namespace dshlite;

  // ── 1. Wire construction: exact body shapes ────────────────────────
  {
    ResponseFormat text;
    check(text.empty() && text.toWire() == nlohmann::json{{"type", "text"}},
          "1: default/text format is empty() and serializes to type:text");
    ResponseFormat jo;
    jo.type = "json_object";
    check(jo.toWire() == nlohmann::json{{"type", "json_object"}},
          "1: json_object wire shape exact");
    ResponseFormat js;
    js.type = "json_schema";
    js.schema = {{"type", "object"}};
    check(js.toWire() == nlohmann::json{{"type", "json_schema"},
                                        {"json_schema", {{"schema", {{"type", "object"}}}}}},
          "1: json_schema wraps under json_schema.schema (gateway contract)");
    ResponseFormat gb;
    gb.type = "gbnf";
    gb.gbnf = "root ::= \"{}\"\n";
    check(gb.toWire() == nlohmann::json{{"type", "gbnf"}, {"grammar", "root ::= \"{}\"\n"}},
          "1: gbnf carries raw grammar text");
  }

  // ── 2. F47 config-time validation mirrors the gateway 400s ────────
  {
    bool threw = false;
    ResponseFormat bad;
    bad.type = "bogus";
    try { bad.toWire(); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "2/F47: invalid type rejected locally (gateway: unsupported_value)");

    threw = false;
    ResponseFormat js;
    js.type = "json_schema";
    js.schema = "not-an-object";
    try { js.toWire(); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "2/F47: json_schema with non-object schema rejected");

    threw = false;
    ResponseFormat gb;
    gb.type = "gbnf";
    gb.gbnf = "   ";
    try { gb.toWire(); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "2/F47: blank gbnf rejected (gateway: non-empty required)");

    threw = false;
    gb.gbnf = "not-root ::= \"x\"\n";
    try { gb.toWire(); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "2/F47: gbnf without root rule rejected (grammar.h law)");

    threw = false;
    gb.gbnf = std::string("root ::= \"x\"") + std::string(2 << 20, ' ');
    try { gb.toWire(); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "2/F47: >1 MiB grammar rejected (gateway: invalid_value)");

    threw = false;
    try {
      toolPayloadFormat({});
    } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "2/F47: toolPayloadFormat with zero tools rejected at build time");
  }

  // ── 3. toolPayloadFormat: F49/F50 schema laws ─────────────────────
  {
    const auto rf = toolPayloadFormat({{"shell", nullptr}, {"read", nullptr}});
    check(rf.type == "json_schema", "3: multi-tool => json_schema");
    const auto& props = rf.schema["properties"];
    check(props["tool"]["enum"] == nlohmann::json::array({"shell", "read"}),
          "3/F50: tool names become an enum span");
    check(!props.contains("args") && rf.schema["required"] == nlohmann::json::array({"tool"}),
          "3/F50: multi-tool args unconstrained (no anyOf in the compiler)");

    const nlohmann::json argsSchema = {
        {"type", "object"},
        {"properties", {{"cmd", {{"type", "string"}}}}},
        {"required", {"cmd"}}};
    const auto single = toolPayloadFormat({{"shell", argsSchema}});
    check(single.schema["properties"].contains("args") &&
              single.schema["properties"]["args"] == argsSchema &&
              single.schema["required"] == nlohmann::json::array({"tool", "args"}),
          "3/F49: single tool inlines args schema, required lists EVERY property");
  }

  // ── 4. toolPayloadGbnf: raw grammar shape ─────────────────────────
  {
    const std::string g = toolPayloadGbnf({"shell", "read"});
    check(g.find("root ::=") == 0, "4: root rule first (grammar.h)");
    check(g.find("\"shell\"") != std::string::npos &&
              g.find("\"read\"") != std::string::npos,
          "4: tool names appear as quoted literals");
    check(g.find("jws ::=") != std::string::npos &&
              g.find("jval ::=") != std::string::npos,
          "4: generic whitespace-tolerant JSON body included");
    ResponseFormat rf;
    rf.type = "gbnf";
    rf.gbnf = g;
    bool valid = true;
    try { validateResponseFormat(rf); } catch (...) { valid = false; }
    check(valid, "4: emitted GBNF passes F47 validation (root rule present)");
  }

  // ── 5. Payload parameter passing on the wire ──────────────────────
  GrammarStub eng;
  eng.start("glm-5.2-colibri", 18900);
  check(eng.port != -1, "5: grammar stub engine up");
  if (eng.port == -1) { std::cout << "GRAMMAR FAIL\n"; return 1; }
  {
    LlmClient llm(stubCfg(eng));
    const std::vector<Message> msgs = {{"user", "propose a tool call"}};
    llm.post(msgs);
    check(eng.lastBody.find("response_format") == std::string::npos,
          "5: plain post() sends NO response_format (pre-G2.4 wire identical)");
    const auto rf = toolPayloadFormat({{"shell", nullptr}});
    llm.postConstrained(msgs, rf);
    const auto sent = nlohmann::json::parse(eng.lastBody);
    check(sent.contains("response_format") &&
              sent["response_format"]["type"] == "json_schema" &&
              sent["response_format"]["json_schema"]["schema"]["properties"]["tool"]["enum"]
                  .size() == 1,
          "5: postConstrained puts response_format on the wire verbatim");
    // text format => plain wire (no field)
    llm.postConstrained(msgs, ResponseFormat{});
    check(eng.lastBody.find("response_format") == std::string::npos,
          "5: text/empty format degenerates to the plain wire");
  }

  // ── 6. F46: capability table + typed refusal + router fallthrough ─
  {
    check(familySupportsGrammar("glm") && !familySupportsGrammar("olmoe") &&
              !familySupportsGrammar("qwen") && !familySupportsGrammar("deepseek") &&
              !familySupportsGrammar("kimi") && !familySupportsGrammar("inkling"),
          "6/F46: grammar_payload table matches family_registry.py (glm only)");

    GrammarStub refusing;
    refusing.refuseGrammar = true;
    refusing.start("olmoe-colibri", 18950);
    {
      LlmConfig c = stubCfg(refusing);
      LlmClient llm(c);
      bool typed = false;
      std::string msg;
      try {
        llm.postConstrained({{"user", "hi"}}, toolPayloadFormat({{"shell", nullptr}}));
      } catch (const GrammarUnsupportedError& e) {
        typed = true;
        msg = e.what();
      }
      check(typed && msg.find("grammar_payload") != std::string::npos,
            "6/F46: live-signature 400 => typed GrammarUnsupportedError");
      // Non-grammar 400s must NOT be mistyped:
      bool plain400 = false;
      LlmConfig c2 = stubCfg(refusing);
      refusing.refuseGrammar = false;  // now answers 200; flip a 404 instead:
      LlmConfig c3 = stubCfg(refusing);
      c3.model = "wrong-model-id";
      LlmClient llm3(c3);
      try {
        llm3.postConstrained({{"user", "hi"}}, toolPayloadFormat({{"shell", nullptr}}));
      } catch (const GrammarUnsupportedError&) {
        plain400 = true;  // would be WRONG
      } catch (const std::runtime_error&) {
        plain400 = false;
      }
      (void)c2;
      check(!plain400, "6/F46: model-id 404 stays a plain error (no mistyping)");
    }
    // Router: refusing first entry, capable second => fallthrough.
    // G1.3 topology law: brain/worker/verifier pools need DISJOINT
    // endpoints — three separate stubs.
    {
      GrammarStub glmBrain, glmWorker, glmVer;
      glmBrain.start("glm-5.2-colibri", 18930);
      glmWorker.start("glm-5.2-colibri", 18940);
      glmVer.start("glm-5.2-colibri", 18945);
      RouterConfig rc;
      EngineEntry bad;
      bad.endpoint = refusing.url();
      bad.modelId = "olmoe-colibri";
      bad.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
      auto mk = [](GrammarStub& s) {
        EngineEntry e;
        e.endpoint = s.url();
        e.modelId = "glm-5.2-colibri";
        e.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
        return e;
      };
      rc.brain = {mk(glmBrain)};
      rc.worker = {bad, mk(glmWorker)};
      rc.verifier = {mk(glmVer)};
      ModelRouter router(rc);
      refusing.refuseGrammar = true;
      auto rr = router.postConstrained(Role::Worker, {{"user", "payload?"}},
                                       toolPayloadFormat({{"shell", nullptr}}));
      check(rr.attempts.size() == 2 && rr.attempts[0].outcome == "grammar-unsupported" &&
                rr.attempts[1].outcome == "ok" && rr.servedBy == glmWorker.url(),
            "6/F46: grammar refusal => own attempt outcome, fallthrough to capable family");
      // All-refusing pool => exhausted throw cites the grammar.
      RouterConfig rc2;
      rc2.brain = {mk(glmBrain)};
      rc2.worker = {bad};
      rc2.verifier = {mk(glmVer)};
      ModelRouter r2(rc2);
      bool cited = false;
      try {
        r2.postConstrained(Role::Worker, {{"user", "payload?"}},
                           toolPayloadFormat({{"shell", nullptr}}));
      } catch (const std::runtime_error& e) {
        cited = std::string(e.what()).find("grammar") != std::string::npos &&
                std::string(e.what()).find("EVERY entry") != std::string::npos;
      }
      check(cited, "6/F46: all-refusing pool => exhausted throw CITES the grammar (fail-loud)");
      glmBrain.stop();
      glmWorker.stop();
      glmVer.stop();
    }
    refusing.stop();
  }

  // ── 7. F45 strict parse ────────────────────────────────────────────
  {
    bool threw = false;
    try { parseStrictPayload("Sure! Here is the call: {\"tool\":\"shell\"}"); }
    catch (const PayloadFormatError&) { threw = true; }
    check(threw, "7/F45: prose-framed JSON rejected (no substring extraction)");

    threw = false;
    try { parseStrictPayload("```json\n{\"tool\":\"shell\"}\n```"); }
    catch (const PayloadFormatError&) { threw = true; }
    check(threw, "7/F45: fenced JSON rejected (no fence stripping)");

    threw = false;
    try { parseStrictPayload("{\"tool\":\"shell\"} trailing junk"); }
    catch (const PayloadFormatError&) { threw = true; }
    check(threw, "7/F45: trailing junk rejected (whole-string parse)");

    threw = false;
    try { parseStrictPayload("   "); } catch (const PayloadFormatError&) { threw = true; }
    check(threw, "7/F45: empty/whitespace rejected");

    const auto j = parseStrictPayload("  {\"tool\":\"shell\",\"args\":{}}  ");
    check(j["tool"] == "shell", "7: clean payload with surrounding whitespace parses");
  }

  // ── 8. solicitToolPayload composition ─────────────────────────────
  {
    FakePoster fake;
    fake.reply = R"({"tool":"shell","args":{"cmd":"true"}})";
    BrainLoop brain(fake, passJudge());
    BrainLoop::HostConfig hc;
    hc.policy.allowedTools = {"shell"};
    brain.setHostConfig(std::move(hc));

    auto sr = brain.solicitToolPayload("propose", {{"shell", nullptr}});
    check(sr.ok && sr.payload["tool"] == "shell" && fake.sawConstrained &&
              fake.lastRf.type == "json_schema",
          "8: solicit sends the payload grammar and strict-parses the reply");
    // The gate receives PRE-VALIDATED structure (objective 1.3):
    const GateVerdict gv = checkPayload(sr.payload, brain.hostConfig().policy);
    check(gv.allowed && gv.code == "OK",
          "8: gate sees pre-validated JSON (structure, not prose)");

    // Prose reply => ok=false, nudge ledger line, NO payload handed on.
    const std::string ledgerPath = "/tmp/hermes-grammar-ledger.jsonl";
    ::remove(ledgerPath.c_str());
    {
      LedgerWriter lw(ledgerPath);
      FakePoster prose;
      prose.reply = "I would run `true` via the shell tool.";
      BrainLoop b2(prose, passJudge());
      BrainLoop::HostConfig hc2;
      hc2.policy.allowedTools = {"shell"};
      hc2.ledger = &lw;
      b2.setHostConfig(std::move(hc2));
      auto bad = b2.solicitToolPayload("propose", {{"shell", nullptr}});
      check(!bad.ok && bad.payload.is_null() &&
                bad.formatError.find("strict JSON") != std::string::npos &&
                bad.raw == prose.reply,
            "8/F45: prose reply => ok=false, raw preserved, no repair attempted");
      std::ifstream in(ledgerPath);
      std::string line;
      bool nudgeLine = false;
      while (std::getline(in, line))
        if (line.find("\"nudge\"") != std::string::npos &&
            line.find("strict parse failed") != std::string::npos)
          nudgeLine = true;
      check(nudgeLine, "8: format failure emits a nudge ledger line (evidence)");
    }
    ::remove(ledgerPath.c_str());

    // F46 fallback: constrained refusal => plain post retry, solicit
    // still works (accelerator must never block solicitation).
    FakePoster refusing;
    refusing.throwGrammarUnsupported = true;
    refusing.reply = R"({"tool":"shell","args":{}})";
    BrainLoop b3(refusing, passJudge());
    auto fb = b3.solicitToolPayload("propose", {{"shell", nullptr}});
    check(fb.ok && refusing.sawPlainAfterRefusal,
          "8/F46: grammar refusal => silent unconstrained retry, solicit succeeds");
  }

  // ── 9. F56 regression: STREAMING 400 must still classify as the typed
  // grammar refusal. On the streaming path the client attaches an SSE
  // content receiver, so a non-200 body is consumed into sse.partial and
  // res->body is EMPTY. Pre-fix, the substring check ran against the
  // empty res->body => generic http-4xx runtime_error (typed error lost,
  // solicit's F46 fallback would NOT fire). Post-fix it reads sse.partial.
  {
    GrammarStub streamRefuse;
    streamRefuse.refuseGrammar = true;
    streamRefuse.start("olmoe-colibri", 19010);
    LlmConfig sc = stubCfg(streamRefuse);
    sc.stream = true;  // the path g4-run actually uses
    LlmClient sllm(sc);
    bool typed = false;
    bool genericHttp4xx = false;
    try {
      sllm.postConstrained({{"user", "hi"}}, toolPayloadFormat({{"shell", nullptr}}));
    } catch (const GrammarUnsupportedError&) {
      typed = true;
    } catch (const std::runtime_error& e) {
      genericHttp4xx = std::string(e.what()).find("HTTP 400") != std::string::npos;
    }
    check(typed,
          "9/F56: streaming 400 (body in sse.partial) => typed GrammarUnsupportedError");
    check(!genericHttp4xx,
          "9/F56: NOT misclassified as generic http-4xx (the pre-fix defect)");
    streamRefuse.stop();
  }

  // ── 10. F60 model-id capability quirk + F61 GLM streaming shape ───
  {
    // F60: glm-5.3-flash is registry family glm53 (grammar_payload=
    // False) while glm-5.2 is family glm (True) — deriveFamily collapses
    // both to "glm", so the id-accurate check must separate them.
    check(modelSupportsGrammar("glm-5.2-colibri") &&
              modelSupportsGrammar("glm-5.3-colibri") &&
              !modelSupportsGrammar("glm-5.3-flash-colibri") &&
              !modelSupportsGrammar("glm-5.3-flash-next-colibri") &&
              !modelSupportsGrammar("olmoe-colibri") &&
              !modelSupportsGrammar("qwen3.8-flash-next-colibri"),
          "10/F60: model-id-accurate grammar capability (glm53-flash excluded)");
    check(familySupportsGrammar("glm") && !familySupportsGrammar("glm53"),
          "10/F60: family table — glm53 is NOT grammar-capable");
  }
  {
    // F61: GLM-family streaming quirk — the gateway emits the <think>
    // span as reasoning_content deltas BEFORE the answer's content
    // deltas (#597 item 4). Laws: reasoning never contaminates content
    // (payload purity), reasoning deltas are counted for telemetry,
    // reasoning bytes still reset stall liveness, and the F33 token
    // estimate counts only content deltas.
    httplib::Server srv;
    std::thread th;
    int port = -1;
    srv.Post("/v1/chat/completions",
             [](const httplib::Request&, httplib::Response& res) {
               res.set_chunked_content_provider(
                   "text/event-stream",
                   [](size_t, httplib::DataSink& sink) {
                     auto send = [&sink](const std::string& frame) {
                       std::string chunk = "data: " + frame + "\n\n";
                       sink.write(chunk.data(), chunk.size());
                     };
                     // Think span first (reasoning_content deltas):
                     send(R"({"choices":[{"delta":{"reasoning_content":"Let me "}}]})");
                     send(R"({"choices":[{"delta":{"reasoning_content":"check the schema."}}]})");
                     // Then the answer as pure content deltas:
                     send(R"({"choices":[{"delta":{"content":"{\"tool\":\"shell\","}}]})");
                     send(R"({"choices":[{"delta":{"content":"\"args\":{\"cmd\":\"echo atom\"}}"}}],)"
                          R"("finish_reason":"stop"})");
                     // Engine-authoritative usage (include_usage honored):
                     send(R"({"choices":[],"usage":{"prompt_tokens":21,)"
                          R"("completion_tokens":9,"total_tokens":30}})");
                     send("[DONE]");
                     sink.done();
                     return true;
                   });
             });
    for (int p = 19100; p < 19140; ++p) {
      httplib::Server probe;
      if (!probe.bind_to_port("127.0.0.1", p)) continue;
      port = p;
      break;
    }
    check(port != -1, "10/F61: GLM-shape streaming stub up");
    if (port != -1) {
      th = std::thread([&] { srv.listen("127.0.0.1", port); });
      for (int i = 0; i < 100 && !srv.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

      LlmConfig c;
      c.endpoint = "http://127.0.0.1:" + std::to_string(port) +
                   "/v1/chat/completions";
      c.model = "glm-5.2-colibri";
      c.apiKeyEnv = "DSHLITE_DEFINITELY_UNSET_ENV_VAR_XYZ";
      c.timeout = std::chrono::seconds(10);
      c.stream = true;
      LlmClient llm(c);
      LlmResponse r = llm.post({{"user", "propose a shell tool call"}});
      check(r.content == R"({"tool":"shell","args":{"cmd":"echo atom"}})",
            "10/F61: reasoning_content NEVER contaminates content (payload pure)");
      check(r.reasoningDeltas == 2 && r.contentDeltas == 2,
            "10/F61: reasoning counted separately from content deltas");
      check(r.usage.completionTokens == 9 && !r.usageEstimated,
            "10/F61: engine usage authoritative; estimate untouched by reasoning");
      // The pure content parses strict — the whole point of the quirk
      // handling: a GLM tool payload survives the think span.
      bool parsed = false;
      try {
        const auto j = parseStrictPayload(r.content);
        parsed = j["tool"] == "shell" && j["args"]["cmd"] == "echo atom";
      } catch (const PayloadFormatError&) {
        parsed = false;
      }
      check(parsed, "10/F61: GLM streamed tool payload strict-parses clean");

      srv.stop();
      if (th.joinable()) th.join();
    }
  }

  eng.stop();
  std::cout << (failures == 0 ? "GRAMMAR PASS\n" : "GRAMMAR FAIL\n");
  return failures == 0 ? 0 : 1;
}