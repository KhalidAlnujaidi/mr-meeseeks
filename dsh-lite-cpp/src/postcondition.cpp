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
PostconditionResult checkOne(const fs::path& ws,
                             const std::string& wsCanonical,
                             const nlohmann::json& cond) {
  if (!cond.is_object()) return fail("condition is not an object");
  if (!cond.contains("file") || !cond["file"].is_string())
    return fail("condition missing string 'file'");
  const std::string fname = cond["file"].get<std::string>();

  // Path traversal guard (defense in depth): the artifact must resolve
  // INSIDE the workspace. Note weakly_canonical tolerates a
  // not-yet-existing tail, which is what lets `exists:false` work.
  std::error_code ec;
  const fs::path raw = ws / fname;
  const fs::path cand = fs::weakly_canonical(raw, ec);
  if (ec) return fail("unresolvable artifact path: " + fname);
  if (cand.string().rfind(wsCanonical, 0) != 0)
    return fail("artifact path escapes workspace: " + fname);

  const bool exists = fs::exists(cand, ec);

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
  if (fs::is_directory(cand, ec)) return fail("artifact is a directory: " + fname);

  const std::string content = slurp(cand);

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
      const PostconditionResult r = checkOne(ws, wsCanon, c);
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

  return checkOne(ws, wsCanon, gt);
}

}  // namespace dshlite
