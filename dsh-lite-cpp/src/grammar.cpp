// grammar.cpp — G2.4: response_format construction/validation, GBNF
// emission, capability table, strict payload parsing. Contract facts in
// grammar.hpp (verified live against coli serve; F45-F50).

#include "dshlite/grammar.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace dshlite {
namespace {

// JSON string literal for GBNF (escapes \" and \\; tool names are
// ASCII identifiers in practice — anything else is rejected at build
// time below rather than silently mis-escaped).
std::string gbnfLit(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      throw std::invalid_argument(
          "grammar: control character in tool name is not representable: " + s);
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

// Generic whitespace-tolerant JSON value grammar — byte-identical
// structure to the gateway's GENERIC_JSON_GBNF (openai_server.py:2435)
// so draft acceptance matches what the engine already ships.
constexpr const char* kGenericJsonBody =
    "jval ::= jobj | jarr | jstr | jnum | \"true\" | \"false\" | \"null\"\n"
    "jobj ::= \"{\" jws ( jstr jws \":\" jws jval jws ( \",\" jws jstr jws \":\" jws jval jws )* )? \"}\"\n"
    "jarr ::= \"[\" jws ( jval jws ( \",\" jws jval jws )* )? \"]\"\n"
    "jstr ::= \"\\\"\" jchar* \"\\\"\"\n"
    "jchar ::= [^\"\\\\\\x00-\\x1f] | \"\\\\\" ( [\"\\\\/bfnrt] | \"u\" jhex jhex jhex jhex )\n"
    "jhex ::= [0-9a-fA-F]\n"
    "jnum ::= \"-\"? ( \"0\" | [1-9] [0-9]* ) ( \".\" [0-9]+ )? ( ( \"e\" | \"E\" ) ( \"+\" | \"-\" )? [0-9]+ )?\n"
    "jws ::= ( \" \" | \"\\t\" | \"\\n\" | \"\\r\" )*\n";

void requireSubset(const std::vector<ToolSchema>& tools) {
  if (tools.empty())
    throw std::invalid_argument(
        "grammar: toolPayloadFormat with zero tools — a payload grammar "
        "with no tool names constrains nothing; refuse at build time (F47)");
  for (const auto& t : tools) {
    if (t.name.empty())
      throw std::invalid_argument("grammar: empty tool name");
    if (!t.args.is_null() && !t.args.is_object())
      throw std::invalid_argument(
          "grammar: args schema for tool '" + t.name +
          "' must be a JSON Schema object or null (F49: the engine "
          "compiler is fail-closed on anything else)");
  }
}

}  // namespace

nlohmann::json ResponseFormat::toWire() const {
  validateResponseFormat(*this);
  if (type == "json_object") return {{"type", "json_object"}};
  if (type == "json_schema")
    return {{"type", "json_schema"}, {"json_schema", {{"schema", schema}}}};
  if (type == "gbnf") return {{"type", "gbnf"}, {"grammar", gbnf}};
  return {{"type", "text"}};  // empty()/text => caller omits the field
}

void validateResponseFormat(const ResponseFormat& rf) {
  // F47: mirror of the gateway's 400s (openai_server.py:2664-2689),
  // same causes, caught before the round-trip.
  if (rf.empty()) return;  // text/absent is always valid
  if (rf.type != "json_object" && rf.type != "json_schema" && rf.type != "gbnf")
    throw std::invalid_argument(
        "response_format.type must be \"text\", \"json_object\", "
        "\"json_schema\" or \"gbnf\" (gateway: unsupported_value) — got: " +
        rf.type);
  if (rf.type == "json_schema") {
    if (!rf.schema.is_object())
      throw std::invalid_argument(
          "response_format.json_schema.schema must be an object "
          "(gateway: invalid_value)");
    const std::string ser = rf.schema.dump();
    if (ser.size() > (1u << 20))
      throw std::invalid_argument(
          "response_format schema exceeds 1 MiB (gateway: invalid_value)");
    if (ser.find('\0') != std::string::npos)
      throw std::invalid_argument("response_format schema contains NUL (gateway: 400)");
  }
  if (rf.type == "gbnf") {
    if (rf.gbnf.empty() ||
        rf.gbnf.find_first_not_of(" \t\n\r") == std::string::npos)
      throw std::invalid_argument(
          "response_format.grammar must be a non-empty GBNF string "
          "(gateway: invalid_value)");
    if (rf.gbnf.size() > (1u << 20))
      throw std::invalid_argument(
          "response_format grammar exceeds 1 MiB (gateway: invalid_value)");
    if (rf.gbnf.find('\0') != std::string::npos)
      throw std::invalid_argument(
          "NUL bytes are not supported in grammars (gateway: 400)");
    // grammar.h: root rule must exist — at the START of a line (a naive
    // substring find would accept "not-root ::=" as a root rule).
    bool hasRoot = false;
    size_t pos = 0;
    while (pos <= rf.gbnf.size()) {
      if (rf.gbnf.compare(pos, 8, "root ::=") == 0) { hasRoot = true; break; }
      pos = rf.gbnf.find('\n', pos);
      if (pos == std::string::npos) break;
      ++pos;
    }
    if (!hasRoot)
      throw std::invalid_argument(
          "grammar: no line-initial 'root ::=' rule — grammar.h requires "
          "the root rule to be named root (F47)");
  }
}

ResponseFormat toolPayloadFormat(const std::vector<ToolSchema>& tools) {
  requireSubset(tools);
  // F49/F50: build the payload schema for {"tool": name, "args": obj}.
  // Multi-tool => tool is an enum of names (the only span we can
  // constrain without anyOf — the compiler rejects it), args stays an
  // unconstrained object. Single tool WITH an args schema => args is
  // constrained by that schema (must be inside the F49 subset or the
  // engine silently drops the grammar — our subset check above only
  // covers the shape we emit; caller-provided schemas are forwarded
  // as-is, matching the gateway's json_schema passthrough).
  nlohmann::json names = nlohmann::json::array();
  for (const auto& t : tools) names.push_back(t.name);

  nlohmann::json argsSchema;
  if (tools.size() == 1 && tools[0].args.is_object()) {
    argsSchema = tools[0].args;
  } else {
    // Unconstrained object. NOTE F50-trap: the engine compiler rejects
    // a bare {"type":"object"} ("object without properties") and an
    // EMPTY properties set compiles to the literal "{}" — both wrong
    // for open-ended args. So the payload schema simply does not
    // constrain args when tools are multi or schemaless: json_schema
    // with properties {tool: enum} and required [tool] only.
    argsSchema = nlohmann::json();  // null => omitted below
  }

  nlohmann::json props = {{"tool", {{"type", "string"}, {"enum", names}}}};
  std::vector<std::string> required = {"tool"};
  if (!argsSchema.is_null()) {
    props["args"] = argsSchema;
    required.push_back("args");
  }
  nlohmann::json schema = {{"type", "object"},
                           {"properties", props},
                           {"required", required}};
  ResponseFormat rf;
  rf.type = "json_schema";
  rf.schema = std::move(schema);
  return rf;
}

std::string toolPayloadGbnf(const std::vector<std::string>& toolNames) {
  if (toolNames.empty())
    throw std::invalid_argument("grammar: toolPayloadGbnf with zero tool names");
  std::string root = "root ::= \"{\" jws \"\\\"tool\\\"\" jws \":\" jws ( ";
  for (size_t i = 0; i < toolNames.size(); ++i) {
    if (i) root += " | ";
    root += gbnfLit(toolNames[i]);
  }
  // Whitespace-tolerant like GENERIC_JSON_GBNF (schema_gbnf.h comment:
  // jws points keep the walker alive through the model's own spacing;
  // a compact-only grammar desyncs and forfeits every later span).
  root += " ) jws ( \",\" jws \"\\\"args\\\"\" jws \":\" jws jval jws )? \"}\" jws\n";
  return root + kGenericJsonBody;
}

bool familySupportsGrammar(const std::string& family) {
  // family_registry.py FamilyCapabilities(tools, grammar_payload,
  // audio_payload, thinking) — grammar_payload=True for glm ONLY
  // (line 1102); olmoe (1202: False,False,False,False... tools=True
  // grammar=False), qwen36/deepseek (1330 comment: "grammars no"),
  // kimi (1171), inkling (audio-only, 1133).
  return family == "glm";
}

nlohmann::json parseStrictPayload(const std::string& content) {
  // F45: the grammar never GUARANTEES shape — this parse is the hard
  // line. Whole-string JSON parse on trimmed content; any prose frame,
  // fence, prefix/suffix garbage => PayloadFormatError (the nudge loop
  // owns retry; no substring extraction, no repair heuristics).
  size_t b = content.find_first_not_of(" \t\n\r");
  if (b == std::string::npos)
    throw PayloadFormatError("payload: empty content — nothing to parse");
  size_t e = content.find_last_not_of(" \t\n\r");
  const std::string trimmed = content.substr(b, e - b + 1);
  try {
    return nlohmann::json::parse(trimmed);  // throws on ANY trailing junk
  } catch (const nlohmann::json::exception& ex) {
    throw PayloadFormatError(std::string("payload: not strict JSON: ") + ex.what());
  }
}

}  // namespace dshlite
