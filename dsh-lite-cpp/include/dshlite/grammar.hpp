#pragma once
// grammar.hpp — G2.4 (roadmap): grammar-forced tool payload drafts for
// local engines via the colibri `response_format` wire contract.
//
// WIRE CONTRACT (verified live against coli serve, openai_server.py
// :2658-2689): body.response_format is one of
//   {"type":"text"}                       (no-op / absent)
//   {"type":"json_object"}                (gateway's generic JSON GBNF)
//   {"type":"json_schema","json_schema":{"schema":{...}}}
//   {"type":"gbnf","grammar":"<raw GBNF text>"}
// The gateway rejects anything else with HTTP 400 (invalid_value /
// unsupported_value), grammar > 1 MiB (invalid_value), NUL bytes in the
// grammar (400), and engines whose family lacks grammar_payload with
// 400 unsupported_parameter (verbatim, observed live on olmoe):
//   {"error":{"message":"`response_format` grammars are not supported
//    by the olmoe engine yet.","type":"invalid_request_error",
//    "param":"response_format","code":"unsupported_parameter"}}
//
// F45 (SEMANTIC LAW — read before promising anything to a user):
// colibri's grammar is a SPECULATIVE DRAFT SOURCE, never a sampling
// constraint (docs/grammar-draft.md; openai_server.py:2658 "NEVER a
// sampling constraint"). Forced spans are verified by the target model;
// a wrong/desynced grammar costs acceptance rate, NOT output shape. So
// a grammar cannot make a model "emit 100% strict JSON" — prose and
// malformed payloads remain possible on any engine. What it DOES buy:
// on grammar-capable families the structural spans (braces, key names,
// enum bodies) are pre-accepted drafts, which measurably accelerates
// structured output and reduces — not eliminates — host nudge loops.
// The G2 payload gate stays the ONLY hard enforcement point.
//
// F46 (capability): grammar_payload is per-family (family_registry.py):
// glm=True; olmoe/qwen/deepseek/kimi/inkling=False. Grammar requests to
// an incapable family 400 at the gateway; the router classifies that as
// its own attempt outcome ("grammar-unsupported") and may fall through
// to a capable family — exhausted pools must cite the grammar rejection
// (fail-loud, never a generic transport error).
//
// F49 (schema subset law): the engine-side compiler (schema_gbnf.h)
// accepts ONLY: object+properties(+required listing EVERY property),
// string(+enum/const), number/integer/boolean/null, array+items
// (+minItems 0|1), annotations ignored, everything else fail-closed
// (NULL => silent no-grammar fallback). F50: no anyOf/oneOf, so a
// multi-tool payload schema can constrain ONLY the tool-name span;
// per-tool args schemas apply when soliciting for one known tool.

#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dshlite {

/// One registered tool: name + an optional args JSON Schema. When args
/// is null, the tool takes an unconstrained object (the grammar then
/// covers only the tool-name span — F50).
struct ToolSchema {
  std::string name;
  nlohmann::json args;  ///< JSON Schema object (subset per F49) or null
};

/// The colibri response_format request field. type "" or "text" means
/// "send nothing" (default — pre-G2.4 wire behavior, byte-identical).
struct ResponseFormat {
  std::string type;         ///< "" | "text" | "json_object" | "json_schema" | "gbnf"
  nlohmann::json schema;    ///< json_schema: the schema OBJECT (wrapped on the wire)
  std::string gbnf;         ///< gbnf: raw grammar text
  bool empty() const { return type.empty() || type == "text"; }
  /// Exact body.response_format value (throws on invalid, F47).
  nlohmann::json toWire() const;
};

/// F47: config-time mirror of the gateway's 400s — same rejection
/// causes, caught locally before a wasted round-trip. Throws
/// std::invalid_argument with a message naming the offending field.
void validateResponseFormat(const ResponseFormat& rf);

/// G2.4 builder: response_format for a tool-call payload
/// {"tool": <one of names>, "args": {...}}. Multi-tool => enum span
/// only (F50). Single tool with an args schema => args constrained too.
/// Empty tool list throws (a payload grammar with no tools is a bug).
ResponseFormat toolPayloadFormat(const std::vector<ToolSchema>& tools);

/// Hand-written raw GBNF for the same payload shape (type "gbnf"):
/// tool-name enum literal + generic JSON value for args, whitespace-
/// tolerant (jws) exactly like the gateway's GENERIC_JSON_GBNF, root
/// rule named "root" (grammar.h requirement).
std::string toolPayloadGbnf(const std::vector<std::string>& toolNames);

/// F46: grammar_payload capability per colibri family
/// (family_registry.py capabilities; glm is the only true today).
/// F60 QUIRK: deriveFamily maps BOTH glm-5.2 (family id "glm",
/// grammar=True) and glm-5.3-flash (family id "glm53", grammar=False)
/// to "glm" — for model-id-accurate capability use
/// modelSupportsGrammar() below; the gateway stays the source of truth
/// either way (it 400s what it cannot compile).
bool familySupportsGrammar(const std::string& family);

/// F60: model-id-aware grammar capability. glm-5.3-flash* => false
/// (registry family glm53, grammar_payload=False); other glm-* ids =>
/// true (family glm, the only grammar_payload=True entry); everything
/// else false. Informational — used to pre-warn, never to gate: an
/// uncapable engine's typed 400 refusal (F46/F51/F56) is authoritative.
bool modelSupportsGrammar(const std::string& modelId);

/// F45 strict parse: the WHOLE trimmed content must be one JSON value —
/// no substring extraction, no fence stripping, no repair. The grammar
/// is a draft accelerator, never a guarantee; anything else throws
/// PayloadFormatError so the caller's nudge loop (not a heuristic)
/// handles retry. Deterministic, fail-loud, never fabricates structure.
struct PayloadFormatError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
nlohmann::json parseStrictPayload(const std::string& content);

}  // namespace dshlite
