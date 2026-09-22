// postcondition.cpp — F72 promotion: artifact-aware verification (layer 2).
// See include/dshlite/postcondition.hpp for history, vocabulary, and laws.

#include "dshlite/postcondition.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace dshlite {
namespace {

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Trailing-newline trim, exactly like the bench-side original and
// referee.py's rstrip("\n").
std::string trimTrailingNewlines(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

PostconditionResult fail(const std::string& why) {
  PostconditionResult r;
  r.pass = false;
  r.evaluated = true;
  r.detail = why;
  return r;
}

PostconditionResult ok(const std::string& why) {
  PostconditionResult r;
  r.pass = true;
  r.evaluated = true;
  r.detail = why;
  return r;
}

/// One condition. Never throws; every failure mode is a result.
/// `wsCanonical` is the workspace root used for the lexical path guard;
/// the workspace path itself is not needed (no canonical resolution —
/// see the guard comment for why).
PostconditionResult checkOne(const std::string& wsCanonical,
                             const nlohmann::json& cond) {
  if (!cond.is_object()) return fail("condition is not an object");
  if (!cond.contains("file") || !cond["file"].is_string())
    return fail("condition missing string 'file'");
  const std::string fname = cond["file"].get<std::string>();

  // Path guard. Two facts force a LEXICAL check rather than a canonical
  // one (F97, caught live on golem/T2):
  //   1. SwarmSpawner SYMLINKS scopeFiles into the workspace
  //      (spawner.cpp:110), so weakly_canonical() resolves a legitimate
  //      in-workspace artifact to its real path OUTSIDE the workspace —
  //      a canonical-prefix test then refuses "hello.txt" as an escape.
  //   2. Canonicalizing also means the check depends on the host
  //      filesystem layout, which a postcondition must not.
  // So: reject absolute paths and any ".." component lexically, then
  // require the LEXICAL path to sit under the workspace root. Symlinks
  // out of the workspace stay readable (that is the spawn contract:
  // scope files ARE the task's inputs, deliberately linked in), while
  // genuine traversal and absolute host paths are refused.
  const fs::path rel(fname);
  if (rel.is_absolute()) return fail("absolute path refused: " + fname);
  for (const auto& part : rel) {
    if (part == "..") return fail("artifact path escapes workspace: " + fname);
  }
  const fs::path lexical = fs::path(wsCanonical) / rel;
  // Defence in depth: the lexical join must still be under the root.
  // (Guarded by the ".." rejection above; kept as a second barrier.)
  if (lexical.lexically_normal().string().rfind(wsCanonical, 0) != 0)
    return fail("artifact path escapes workspace: " + fname);

  std::error_code ec;
  // symlink_status: do NOT follow the link when asking whether the
  // artifact exists — a dangling scope symlink is a real failure, but an
  // intact one pointing outside the workspace is a legitimate fixture.
  const bool exists = fs::exists(lexical, ec);

  // exists:<bool> — explicit existence predicate, including negation
  // (the T9/T10 canary assertions rely on `exists: false`).
  if (cond.contains("exists")) {
    if (!cond["exists"].is_boolean())
      return fail("'exists' must be a boolean");
    const bool want = cond["exists"].get<bool>();
    if (exists == want)
      return ok(fname + " exists=" + (exists ? "true" : "false") +
                " want=" + (want ? "true" : "false"));
    return fail(fname + " exists=" + (exists ? "true" : "false") +
                " want=" + (want ? "true" : "false"));
  }

  if (!exists) return fail("artifact missing: " + fname);
  if (fs::is_directory(lexical, ec)) return fail("artifact is a directory: " + fname);

  const std::string content = slurp(lexical);

  if (cond.contains("equals") && cond["equals"].is_string()) {
    const std::string got = trimTrailingNewlines(content);
    const std::string want = cond["equals"].get<std::string>();
    if (got == want) return ok(fname + " equals-verified");
    return fail(fname + " content mismatch (want equals \"" + want +
                "\", got \"" + got.substr(0, 64) + "\")");
  }

  if (cond.contains("contains") && cond["contains"].is_string()) {
    const std::string want = cond["contains"].get<std::string>();
    if (content.find(want) != std::string::npos)
      return ok(fname + " contains-verified");
    return fail(fname + " content mismatch (missing required substring)");
  }

  // regex: multiline, ECMAScript — matches referee.py re.search(r, content, re.M).
  if (cond.contains("regex") && cond["regex"].is_string()) {
    const std::string rx = cond["regex"].get<std::string>();
    std::regex re;
    try {
      re = std::regex(rx, std::regex::ECMAScript | std::regex::multiline);
    } catch (const std::regex_error& e) {
      // A malformed host-declared regex is a HOST bug: fail loud, never
      // silently pass (that would be a false "verified").
      return fail("invalid regex \"" + rx + "\": " + e.what());
    }
    if (std::regex_search(content, re)) return ok(fname + " regex-verified");
    return fail(fname + " regex no match: " + rx);
  }

  // file-exists-only (legacy C++ behavior: no check kind given).
  return ok(fname + " exists-verified");
}

}  // namespace

PostconditionResult checkPostcondition(const std::string& workspaceDir,
                                       const nlohmann::json& gt) {
  if (gt.is_null() || gt.empty()) {
    PostconditionResult r;
    r.pass = true;
    r.evaluated = false;  // nothing declared — NEVER reported as verified
    r.detail = "no-postcondition (exit-code only)";
    return r;
  }

  std::error_code ec;
  const fs::path ws(workspaceDir);
  const fs::path wsCanonical = fs::weakly_canonical(ws, ec);
  if (ec)
    throw std::runtime_error("checkPostcondition: unresolvable workspace: " +
                             workspaceDir);
  const std::string wsCanon = wsCanonical.string();

  // all_of: conjunction (referee.py check_ground_truth).
  if (gt.contains("all_of")) {
    if (!gt["all_of"].is_array()) return fail("'all_of' must be an array");
    PostconditionResult agg = ok("all_of: all conditions held");
    std::string details;
    bool all = true;
    for (const auto& c : gt["all_of"]) {
      const PostconditionResult r = checkOne(wsCanon, c);
      all = all && r.pass;
      if (!details.empty()) details += "; ";
      details += r.detail;
      if (!r.pass) break;  // first failure is the reported reason
    }
    agg.pass = all;
    agg.evaluated = true;
    agg.detail = "all_of: " + details;
    return agg;
  }

  return checkOne(wsCanon, gt);
}

}  // namespace dshlite
