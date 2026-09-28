// payload_draft_run.cpp — Golem-on-enigma draft-rate bench (live engine).
//
// QUESTION this answers: Golem's tool intent travels over
// body.response_format (grammar-forced drafts), NOT the OpenAI `tools`
// channel (llm_client.cpp:108-123 has no tools field; brain.cpp:313-346
// solicits a {"tool":...,"args":...} payload under a grammar). enigma
// serves qwen3.6-colibri, whose family is grammar_payload=False
// (grammar.hpp F46) — so the grammar request 400s and brain.cpp:326
// falls back to a PLAIN post(). The draft is then UNCONSTRAINED prose
// that parseStrictPayload() (grammar.cpp:203-218, whole-string JSON)
// must accept cleanly.
//
// So: how often does qwen36 draft a gate-valid payload with NO grammar
// acceleration? That rate is what decides whether Golem functions on
// this engine, and it is NOT answerable by reading code.
//
// NOT measured / NOT claimed:
//   - Not a statement that "Golem works/breaks on enigma" — only a
//     measured draft rate for this engine family (github F45: the
//     grammar was never a sampling constraint, so its absence costs
//     acceptance rate, not output shape).
//   - The leaf prompt names tools WITHOUT arg schemas (loop.ts:313-321
//     LEAF_HEADER + ALLOWLISTS are bare names), so "args" quality is
//     capped by an UPSTREAM harness property, not by this bench.
//
// Usage: ./build/payload-draft-run <endpoint-url> <model-id> [probes]
//   e.g. ./build/payload-draft-run \
//          http://enigma:8000/v1/chat/completions qwen3.6-colibri 8
//
// Exit code 0 = bench completed (rate printed); the RATE is the result,
// never an assertion — a 0.0 rate is a legitimate measurement.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dshlite/brain.hpp"
#include "dshlite/grammar.hpp"
#include "dshlite/llm_client.hpp"
#include "dshlite/payload_gate.hpp"

using namespace dshlite;
using nlohmann::json;

namespace {

// The frozen leaf header from harness/loop.ts:265-269 (verbatim).
// Duplicated here as a literal so this bench is hermetic — it never
// shells out to node. Kept byte-identical to LEAF_HEADER so the probe
// reflects the real Golem leaf prompt frame.
constexpr const char* kLeafHeader =
    "You are a leaf worker. You see ONLY your task below. Do exactly one "
    "thing: either DO it and report back, or (if non-atomic) answer "
    "NEEDS_SPLIT plus one reason and stop. Never do both. Never re-split "
    "someone else's atomic unit. You never create subtasks; only the loop "
    "splits.";

// One probe: a task criterion phrased the way buildLeafInvocation would
// (loop.ts:316-319), paired with the role whose allowlist it exercises.
//
// F6 (contamination): prompts are deliberately LEXICALLY DISTINCT across
// probes — no shared boilerplate beyond the frozen header — because
// COLI_CACHE=1 (0.93) answers near-duplicate prompts WITHOUT running the
// engine, which would silently replace real decode with a cache hit.
struct Probe {
  const char* role;
  const char* criterion;
};

// ---------------------------------------------------------------------
// PROMPT VARIANTS (the actual experiment)
//
// READING THE PRODUCTION CALL-PATH CHANGED THIS BENCH. Both real Golem
// drivers construct a SHAPE-DECLARING solicitation prompt: name the
// target, show a BARE UNFENCED example of the exact reply, and say
// "reply with only the JSON object" (g4_run.cpp:377-384,
// mixed_lane_run.cpp:137-143). g4_run.cpp:369-375 records WHY as a live
// finding:
//
//   F58 (live finding): OLMoE fences whatever shape the prompt's last
//   example showed — an inline schema in prose elicited ```json fences
//   on 6/6 attempts (nudge text included), while a BARE unfenced
//   example line elicited raw JSON first-try. The model imitates
//   surface form, so the prompt must model the surface form.
//
// Meanwhile harness/loop.ts buildLeafInvocation() (the TS loop) builds a
// REPORT prompt — "Answer with your result, or with NEEDS_SPLIT:<reason>"
// — which never asks for a payload and never shows its shape. Feeding
// THAT prompt to solicitToolPayload asks the model for one thing and
// grades it on another.
//
// So the bench runs BOTH as arms against the same engine and tools:
//   arm "report"    — the loop.ts buildLeafInvocation frame (control)
//   arm "solicit"   — the g4_run.cpp shape-declaring frame (treatment)
// The delta between the two arm rates IS the measurement.
// ---------------------------------------------------------------------
enum class PromptVariant { kReportFrame, kSolicitFrame };

const char* variantName(PromptVariant v) {
  return v == PromptVariant::kReportFrame ? "report" : "solicit";
}

// The g4_run.cpp:377-384 frame, generalised: same surface form
// (example line, no fences, explicit "only the JSON object"), with the
// example tool drawn from the ACTUAL allowlist so the model imitates a
// shape that can pass the gate.
std::string solicitFrame(const std::string& role,
                         const std::string& criterion,
                         const std::vector<ToolSchema>& tools) {
  // Pick the first allowlisted tool and a plausible args key for it.
  const std::string t0 = tools.empty() ? "read" : tools[0].name;
  std::string exampleArgs = "{\"path\":\"notes.txt\"}";
  if (t0 == "bash") exampleArgs = "{\"cmd\":\"echo hello\"}";
  else if (t0 == "web_search") exampleArgs = "{\"query\":\"release date\"}";
  else if (t0 == "web_fetch") exampleArgs = "{\"url\":\"https://example.com\"}";
  else if (t0 == "grep") exampleArgs = "{\"pattern\":\"TODO\"}";
  else if (t0 == "glob") exampleArgs = "{\"pattern\":\"*.hpp\"}";
  else if (t0 == "edit") exampleArgs = "{\"path\":\"README.md\"}";
  return "Propose exactly one tool call that accomplishes this task: " +
         criterion +
         "\nYou may use only this tool: " + t0 +
         "\nExample of the exact expected reply format:\n"
         "{\"tool\":\"" + t0 + "\",\"args\":" + exampleArgs + "}\n"
         "Now propose the call. Reply with only the JSON object.";
}

const std::vector<Probe> kProbes = {
    {"engineer", "Read the build log at /tmp/build.log and report the exact line number where the first compiler error appears."},
    {"researcher", "Find the release date of the Qwen3 technical report and quote the sentence that states it."},
    {"reviewer", "Count how many times the identifier ai_decode appears in dsh-lite-cpp/src/abi_client.cpp."},
    {"engineer", "Run the ctest suite named stub-llm and report the single word that appears in its last line of output."},
    {"researcher", "Locate the license file at the repository root and quote its first non-empty line verbatim."},
    {"reviewer", "List every TODO comment marker present in dsh-lite-cpp/include/dshlite/nudge.hpp, quoting each in full."},
    {"engineer", "Edit dsh-lite-cpp/README.md to change the phrase zero-dependency to no-dependency and nothing else."},
    {"researcher", "Fetch the colibri project page and report the name of its default smallest model family identifier."},
    {"reviewer", "Determine whether the string GOLEM_LEDGER is read by the driver in dsh-lite-cpp/tests/g4_run.cpp."},
    {"engineer", "Read dsh-lite-cpp/include/dshlite/spawner.hpp and report the exact watchdog timeout in seconds."},
};

// Role allowlists, verbatim from loop.ts:292-296.
std::vector<ToolSchema> toolsForRole(const std::string& role) {
  std::vector<std::string> names;
  if (role == "researcher")
    names = {"web_search", "web_fetch", "read"};
  else if (role == "engineer")
    names = {"read", "edit", "bash"};
  else if (role == "reviewer")
    names = {"read", "grep", "glob"};
  std::vector<ToolSchema> out;
  for (const auto& n : names) out.push_back(ToolSchema{n, json()});
  return out;
}

// Reconstruct the real leaf prompt frame (loop.ts:316-320). Role prompt
// text is the short form of ROLE_PROMPTS — the frame shape is what
// matters for the draft, and the tools list is appended as the harness
// does via ALLOWLISTS.
std::string leafPrompt(const std::string& role, const std::string& criterion,
                       const std::vector<ToolSchema>& tools) {
  std::string names;
  for (size_t i = 0; i < tools.size(); ++i) {
    if (i) names += ", ";
    names += tools[i].name;
  }
  return std::string(kLeafHeader) + "\n\nROLE (" + role +
         "): You are the " + role +
         " for exactly one task.\n\nTASK t.x (depth 0): " + criterion +
         "\nAvailable tools (exact names): " + names +
         "\nAnswer with your result, or with \"NEEDS_SPLIT:<reason>\" if "
         "this needs more than one tool call.";
}

// Outcome buckets. F3 (separation law): "emitted JSON that is not the
// gate schema" MUST NOT be conflated with "emitted no JSON" — they have
// different causes and different fixes.
enum class Outcome {
  kGateOk,          // strict JSON + {"tool":str,"args":obj} + allowlisted
  kPolicyRefused,   // strict JSON, valid shape, but gate refused/denied
  kBadShape,        // strict JSON, WRONG shape (e.g. no args object)
  kNoJson,          // parseStrictPayload threw: prose / fences / junk
  kTransportError,  // threw before parsing (engine/transport)
};

const char* outcomeName(Outcome o) {
  switch (o) {
    case Outcome::kGateOk: return "GATE_OK";
    case Outcome::kPolicyRefused: return "POLICY_REFUSED";
    case Outcome::kBadShape: return "BAD_SHAPE";
    case Outcome::kNoJson: return "NO_JSON";
    case Outcome::kTransportError: return "TRANSPORT_ERROR";
  }
  return "?";
}

// F1 (first-probe classification check): the F46 fallback path depends
// on the engine's 400 being recognized as GrammarUnsupportedError. If
// that classification is wrong the bench dies on probe 1 — so probe 1
// reports WHICH exception type surfaced, as evidence.
struct Counters {
  int gateOk = 0, policyRefused = 0, badShape = 0, noJson = 0, transport = 0;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0]
              << " <endpoint-url> <model-id> [probes]\n"
              << "  e.g. " << argv[0]
              << " http://enigma:8000/v1/chat/completions qwen3.6-colibri 8\n";
    return 2;
  }
  const std::string endpoint = argv[1];
  const std::string model = argv[2];
  int n = (argc > 3) ? std::atoi(argv[3]) : 8;
  if (n < 1) n = 1;
  if (n > static_cast<int>(kProbes.size())) n = static_cast<int>(kProbes.size());

  LlmConfig cfg;
  cfg.endpoint = endpoint;
  cfg.model = model;               // verbatim: engine 404s anything else
  cfg.maxTokens = 384;             // a payload draft is short by design
  cfg.timeout = std::chrono::milliseconds(180000);  // i4 + CPU offload is slow
  cfg.stream = false;
  // No apiKey: enigma runs keyless (COLI_ALLOW_INSECURE_BIND=1). A
  // non-loopback host with an empty key would throw at postImpl:100-103,
  // so a key env is set to a harmless sentinel to exercise the real
  // non-loopback bearer path exactly as a deployed Golem worker would.
  cfg.apiKeyEnv = "COLI_API_KEY";
  cfg.apiKey = std::getenv("COLI_API_KEY") ? std::string() : std::string("colibri-local");

  LlmClient llm(cfg);
  // Judge hook: unused by solicitToolPayload (solicitation is a leaf
  // question and never consults the judge), so a benign always-escalate
  // stand-in keeps the constructor honest without a network dependency.
  BrainLoop brain(llm, [](const std::string&) {
    JudgeVerdict v;
    v.action = JudgeAction::Escalate;
    v.fallback = true;
    v.detail = "bench: judge hook not exercised";
    return v;
  });

  PolicyConfig policy;
  for (const auto& p : kProbes)
    for (const auto& t : toolsForRole(p.role))
      policy.allowedTools.push_back(t.name);

  std::cout << "=== Golem-on-enigma payload draft-rate bench ===\n";
  std::cout << "endpoint : " << endpoint << "\n";
  std::cout << "model    : " << model << "\n";
  std::cout << "probes   : " << n << " of " << kProbes.size() << "\n";
  std::cout << "maxTokens: " << cfg.maxTokens << "\n";
  std::cout << "ARMS: report (loop.ts buildLeafInvocation frame, control)\n"
               "      solicit (g4_run.cpp shape-declaring frame, treatment)\n";
  std::cout << "NOTE: grammar_payload=False for this family (grammar.hpp\n"
               "      F46), so the response_format request 400s and the\n"
               "      F46 fallback at brain.cpp:326 retries WITHOUT the\n"
               "      grammar. Both arms therefore measure the\n"
               "      UNCONSTRAINED rate; the DELTA is the experiment.\n\n";

  Counters c;
  // Raw evidence log (one JSON object per probe) — written to stdout so
  // the run is auditable without re-running the engine.
  std::cout << "--- raw evidence ---\n";

  for (int i = 0; i < n; ++i) {
    const Probe& p = kProbes[i];
    const std::vector<ToolSchema> tools = toolsForRole(p.role);
    // CONTROL arm: the loop.ts buildLeafInvocation report frame.
    const std::string prompt = leafPrompt(p.role, p.criterion, tools);

    Outcome outcome = Outcome::kTransportError;
    std::string raw;
    std::string detail;
    std::string excType = "none";

    try {
      BrainLoop::SolicitResult r = brain.solicitToolPayload(prompt, tools);
      raw = r.raw;
      if (r.ok) {
        // Strict JSON parsed. Now apply the REAL gate.
        GateVerdict v = checkPayload(r.payload, policy);
        if (v.allowed && autoExecutable(v)) {
          outcome = Outcome::kGateOk;
        } else if (v.allowed) {
          outcome = Outcome::kPolicyRefused;
          detail = v.code;
        } else {
          // Gate refused. Distinguish schema-shape failure (F3) from
          // policy refusal: PAYLOAD_SCHEMA_INVALID == wrong shape.
          if (v.code == "PAYLOAD_SCHEMA_INVALID") {
            outcome = Outcome::kBadShape;
            detail = v.message;
          } else {
            outcome = Outcome::kPolicyRefused;
            detail = v.code;
          }
        }
      } else {
        outcome = Outcome::kNoJson;
        detail = r.formatError;
      }
    } catch (const GrammarUnsupportedError& e) {
      // F1: this would mean the F46 fallback DID NOT catch the live
      // refusal — a real finding, not a bench failure.
      excType = "GrammarUnsupportedError";
      detail = e.what();
    } catch (const std::exception& e) {
      excType = "std::runtime_error";
      detail = e.what();
    } catch (...) {
      excType = "unknown";
    }

    switch (outcome) {
      case Outcome::kGateOk: ++c.gateOk; break;
      case Outcome::kPolicyRefused: ++c.policyRefused; break;
      case Outcome::kBadShape: ++c.badShape; break;
      case Outcome::kNoJson: ++c.noJson; break;
      case Outcome::kTransportError: ++c.transport; break;
    }

    json ev;
    ev["probe"] = i + 1;
    ev["role"] = p.role;
    ev["criterion"] = p.criterion;
    ev["tools"] = json::array();
    for (const auto& t : tools) ev["tools"].push_back(t.name);
    ev["outcome"] = outcomeName(outcome);
    if (!detail.empty()) ev["detail"] = detail.substr(0, 300);
    if (excType != "none") ev["exception"] = excType;
    ev["raw"] = raw.substr(0, 600);  // evidence, truncated for readability
    ev["raw_len"] = static_cast<long>(raw.size());
    std::cout << ev.dump() << "\n";
    std::cout.flush();

    if (outcome == Outcome::kTransportError && excType == "std::runtime_error" &&
        detail.find("HTTP 400") != std::string::npos) {
      // The F46 fallback failed to classify the live refusal. Stop: the
      // measurement is not meaningful and the cause needs fixing first.
      std::cerr << "\nFATAL (F1): a 400 escaped the GrammarUnsupportedError "
                   "fallback — the refusal was misclassified. Bench "
                   "aborted rather than reporting a meaningless rate.\n";
      return 3;
    }
  }

  const int total = c.gateOk + c.policyRefused + c.badShape + c.noJson + c.transport;
  const int attempted = total - c.transport;
  std::cout << "\n--- result: ARM report (control, loop.ts frame) ---\n";
  std::cout << "probes_completed      : " << total << "\n";
  std::cout << "transport_errors      : " << c.transport << "\n";
  std::cout << "GATE_OK               : " << c.gateOk << "\n";
  std::cout << "BAD_SHAPE (json, not  : " << c.badShape << "  <- F3: JSON but\n";
  std::cout << "  gate shape)           wrong object shape\n";
  std::cout << "POLICY_REFUSED        : " << c.policyRefused << "\n";
  std::cout << "NO_JSON (prose/fence) : " << c.noJson << "\n";
  if (attempted > 0) {
    const double rate = 100.0 * static_cast<double>(c.gateOk) /
                        static_cast<double>(attempted);
    std::cout << "\nDRAFT RATE (report arm): "
              << c.gateOk << "/" << attempted << " = " << rate << "%\n";
    const double parseable =
        100.0 * static_cast<double>(c.gateOk + c.badShape + c.policyRefused) /
        static_cast<double>(attempted);
    std::cout << "STRICT-JSON RATE (report arm): "
              << c.gateOk + c.badShape + c.policyRefused << "/" << attempted
              << " = " << parseable << "%\n";
  }

  // ------------------------------------------------------------------
  // ARM 2 (treatment): the g4_run.cpp shape-declaring solicitation frame.
  // Same engine, same tools, same gate. Only the prompt frame differs,
  // so the delta between arm rates isolates the prompt's contribution.
  // ------------------------------------------------------------------
  std::cout << "\n--- raw evidence: solicit arm (treatment) ---\n";
  Counters s;
  for (int i = 0; i < n; ++i) {
    const Probe& p = kProbes[i];
    const std::vector<ToolSchema> tools = toolsForRole(p.role);
    const std::string prompt = solicitFrame(p.role, p.criterion, tools);

    Outcome outcome = Outcome::kTransportError;
    std::string raw, detail, excType = "none";
    try {
      BrainLoop::SolicitResult r = brain.solicitToolPayload(prompt, tools);
      raw = r.raw;
      if (r.ok) {
        GateVerdict v = checkPayload(r.payload, policy);
        if (v.allowed && autoExecutable(v)) {
          outcome = Outcome::kGateOk;
        } else if (v.allowed) {
          outcome = Outcome::kPolicyRefused;
          detail = v.code;
        } else if (v.code == "PAYLOAD_SCHEMA_INVALID") {
          outcome = Outcome::kBadShape;
          detail = v.message;
        } else {
          outcome = Outcome::kPolicyRefused;
          detail = v.code;
        }
      } else {
        outcome = Outcome::kNoJson;
        detail = r.formatError;
      }
    } catch (const GrammarUnsupportedError& e) {
      excType = "GrammarUnsupportedError";
      detail = e.what();
    } catch (const std::exception& e) {
      excType = "std::runtime_error";
      detail = e.what();
    } catch (...) {
      excType = "unknown";
    }

    switch (outcome) {
      case Outcome::kGateOk: ++s.gateOk; break;
      case Outcome::kPolicyRefused: ++s.policyRefused; break;
      case Outcome::kBadShape: ++s.badShape; break;
      case Outcome::kNoJson: ++s.noJson; break;
      case Outcome::kTransportError: ++s.transport; break;
    }

    json ev;
    ev["arm"] = "solicit";
    ev["probe"] = i + 1;
    ev["role"] = p.role;
    ev["outcome"] = outcomeName(outcome);
    if (!detail.empty()) ev["detail"] = detail.substr(0, 300);
    if (excType != "none") ev["exception"] = excType;
    ev["raw"] = raw.substr(0, 600);
    ev["raw_len"] = static_cast<long>(raw.size());
    std::cout << ev.dump() << "\n";
    std::cout.flush();
  }

  const int stotal = s.gateOk + s.policyRefused + s.badShape + s.noJson + s.transport;
  const int sattempted = stotal - s.transport;
  std::cout << "\n--- result: ARM solicit (treatment, g4_run frame) ---\n";
  std::cout << "probes_completed      : " << stotal << "\n";
  std::cout << "transport_errors      : " << s.transport << "\n";
  std::cout << "GATE_OK               : " << s.gateOk << "\n";
  std::cout << "BAD_SHAPE (json, not  : " << s.badShape << "\n";
  std::cout << "  gate shape)           wrong object shape\n";
  std::cout << "POLICY_REFUSED        : " << s.policyRefused << "\n";
  std::cout << "NO_JSON (prose/fence) : " << s.noJson << "\n";

  std::cout << "\n=== HEADLINE: prompt frame delta (qwen3.6-colibri, no grammar) ===\n";
  std::cout << "control  (report frame)  : " << c.gateOk << "/" << attempted
            << " gate-valid payloads\n";
  std::cout << "treatment(solicit frame) : " << s.gateOk << "/" << sattempted
            << " gate-valid payloads\n";
  if (attempted > 0 && sattempted > 0) {
    const double cr = 100.0 * c.gateOk / attempted;
    const double sr = 100.0 * s.gateOk / sattempted;
    std::cout << "rates                    : report " << cr << "%  vs  solicit "
              << sr << "%\n";
    if (sr > cr)
      std::cout << "PROMPT FRAME IS THE BINDING CONSTRAINT: declaring the\n"
                   "payload shape raised the rate without any grammar, any\n"
                   "engine change, or any model swap.\n";
    else
      std::cout << "NO PROMPT-FRAME EFFECT MEASURED on this engine at this n.\n"
                   "The shape declaration did not move the rate.\n";
  }
  std::cout << "\nBoth arms ran with grammar_payload=False (F46), so neither\n"
               "number reflects grammar acceleration. Evidence: the raw\n"
               "lines above; this is a measurement, not an assertion.\n";
  return 0;
}
