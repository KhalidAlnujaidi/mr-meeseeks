// compaction.cpp — Gap 2 (G2.5/F1): history compaction + E.2.3 result
// vectors. See include/dshlite/compaction.hpp for the contract.

#include "dshlite/compaction.hpp"

#include <algorithm>
#include <cctype>

#include "dshlite/sanitizer.hpp"

namespace dshlite {
namespace {

std::string toUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return s;
}

long countLines(const std::string& s) {
  if (s.empty()) return 0;
  long n = 1;
  for (char c : s)
    if (c == '\n') ++n;
  // A trailing newline does not open a 6th "line" of content.
  if (s.back() == '\n') --n;
  return std::max<long>(n, s.empty() ? 0 : 1);
}

ResultVectorVerdict reject(const std::string& code, const std::string& msg) {
  ResultVectorVerdict v;
  v.ok = false;
  v.code = code;
  v.message = msg;
  return v;
}

}  // namespace

long serializedChars(const std::vector<Message>& h) {
  long total = 0;
  for (const auto& m : h)
    total += static_cast<long>(m.role.size() + m.content.size() + 4);
  return total;
}

bool compactHistory(std::vector<Message>& h, const CompactionConfig& cfg) {
  if (h.empty()) return false;
  const long threshold = cfg.thresholdChars;
  if (serializedChars(h) <= threshold) return false;  // under budget: no-op

  // Protected set (G2.5 law): index 0 when it is the system message,
  // the final message, and the LAST user message. Everything between is
  // prunable. (If index 0 is not "system" it stays prunable — the law
  // names the system message, not the position blindly.)
  const size_t n = h.size();
  auto isProtected = [&](size_t i) {
    if (i == 0 && h[0].role == "system") return true;
    if (i == n - 1) return true;  // final message, whatever its role
    // last user message:
    for (size_t j = n; j-- > 0;) {
      if (h[j].role == "user") return i == j;
    }
    return false;
  };

  bool changed = false;

  // Stage 1 — per-message head/tail truncation of prunable turns.
  // Every touched message is sanitized FIRST (Module 1 firebreak law),
  // then truncated to head(headChars) + marker + tail(tailChars).
  // F23 (honest note): sanitize() already caps single messages at
  // ~4096 chars (head2048+marker+tail1024), so at DEFAULT cfg sizes
  // (4096+1024) this stage can only bite when the caller lowers the
  // caps — whole-turn pruning (stage 2) is the load-bearing mechanism.
  for (size_t i = 0; i < n; ++i) {
    if (isProtected(i)) continue;
    std::string clean = sanitize(h[i].content);
    const long cap = cfg.headChars + cfg.tailChars;
    if (static_cast<long>(clean.size()) > cap && cap > 0) {
      std::string cut;
      cut.append(clean, 0, static_cast<size_t>(cfg.headChars));
      cut.append(TRUNCATION_MARKER);
      cut.append(clean.data() + clean.size() - cfg.tailChars,
                 static_cast<size_t>(cfg.tailChars));
      clean = std::move(cut);
    }
    if (clean != h[i].content) {
      h[i].content = std::move(clean);
      changed = true;
    }
  }
  if (serializedChars(h) <= threshold) return changed;

  // Stage 2 — drop middle turns oldest-first until under budget.
  // The drop target RESERVES room for the [HOST COMPACTION] marker
  // inserted afterwards (F-fix: stopping exactly at threshold and then
  // adding ~170 chars of marker would push back over budget — the
  // compacted history must fit WITH its provenance marker). Reserve is
  // clamped so tiny thresholds (tests) still make progress.
  const long kMarkerReserve = 256;  // >= any marker text at default caps
  const long reserve = std::min<long>(kMarkerReserve, threshold / 2);
  const long target = threshold - reserve;

  std::vector<size_t> prunable;
  for (size_t i = 0; i < n; ++i)
    if (!isProtected(i)) prunable.push_back(i);

  long dropped = 0;
  size_t firstDroppedIdx = n;  // marker insertion point
  for (size_t pi = 0; pi < prunable.size(); ++pi) {
    if (dropped > 0 && serializedChars(h) <= target) break;
    const size_t idx = prunable[pi] - static_cast<size_t>(dropped);
    if (idx >= h.size()) break;  // defensive: indices shift as we erase
    if (firstDroppedIdx == n) firstDroppedIdx = idx;
    h.erase(h.begin() + static_cast<long>(idx));
    ++dropped;
    changed = true;
  }
  if (dropped > 0) {
    // One host-attributed marker replaces the pruned region — explicit
    // provenance ("[HOST COMPACTION]", role "user"): never disguised as
    // model output. Sanitized like every re-injected string (G2.5).
    Message marker;
    marker.role = "user";
    marker.content = sanitize(
        std::string(kCompactionMarker) + " " + std::to_string(dropped) +
        " middle turn(s) pruned to protect brain context (G2.5, budget " +
        std::to_string(threshold) +
        " chars) — full detail lives in ledger lines + patch artifacts, "
        "not here");
    const size_t at = std::min(firstDroppedIdx, h.size());
    h.insert(h.begin() + static_cast<long>(at), std::move(marker));
  }
  return changed;
}

ResultVectorVerdict toResultVector(const nlohmann::json& in, bool hasPatchFile) {
  if (!in.is_object())
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: input must be a JSON object (E.2.3) — got " +
                      std::string(in.type_name()));
  ResultVectorVerdict out;
  ResultVector& v = out.vector;

  // taskId: string, required, non-empty.
  if (!in.contains("taskId") || !in["taskId"].is_string() ||
      in["taskId"].get<std::string>().empty())
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: \"taskId\" (non-empty string) required "
                  "(E.2.3) — recovery: leaf resubmits with the originating "
                  "task id");
  v.taskId = sanitize(in["taskId"].get<std::string>());

  // status: canonical enum, lowercase input normalized, else REJECTED.
  if (!in.contains("status") || !in["status"].is_string())
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: \"status\" (string) required (E.2.3)");
  v.status = toUpper(in["status"].get<std::string>());
  if (v.status != kStatusSuccess && v.status != kStatusFailed &&
      v.status != kStatusDiscarded)
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: status \"" + v.status +
                      "\" not in SUCCESS|FAILED|DISCARDED (E.2.3 canonical "
                      "enum; lowercase is normalized, anything else is "
                      "rejected) — recovery: leaf resubmits");

  // summary: string, 1-5 lines, sanitized.
  if (!in.contains("summary") || !in["summary"].is_string())
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: \"summary\" (string, 1-5 lines) required "
                  "(E.2.3)");
  v.summary = sanitize(in["summary"].get<std::string>());
  const long lines = countLines(v.summary);
  if (lines < 1 || lines > 5)
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: summary must be 1-5 lines (E.2.3) — got " +
                      std::to_string(lines) +
                      "; recovery: leaf compresses; stack traces/raw logs "
                      "belong in the pruned transcript + patch, not here");

  // changedFiles: array of repo-relative path strings, may be empty.
  if (!in.contains("changedFiles") || !in["changedFiles"].is_array())
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: \"changedFiles\" (array, may be empty) "
                  "required (E.2.3)");
  for (const auto& f : in["changedFiles"]) {
    if (!f.is_string())
      return reject("VECTOR_FIELD_INVALID",
                    "result vector: changedFiles entries must be strings "
                    "(repo-relative paths, E.2.3)");
    const std::string path = sanitize(f.get<std::string>());
    if (path.empty() || path[0] == '/')
      return reject("VECTOR_FIELD_INVALID",
                    "result vector: changedFiles entry \"" + path +
                        "\" is not a repo-relative path (E.2.3) — absolute "
                        "paths are refused");
    v.changedFiles.push_back(path);
  }

  // gitDiffHash: conditional binding (E.2.3): REQUIRED non-empty iff a
  // patch exists; "" is the canonical no-patch value (B.5/D.4 withhold).
  std::string hash;
  if (in.contains("gitDiffHash") && !in["gitDiffHash"].is_null()) {
    if (!in["gitDiffHash"].is_string())
      return reject("VECTOR_FIELD_INVALID",
                    "result vector: \"gitDiffHash\" must be a string (E.2.3)");
    hash = sanitize(in["gitDiffHash"].get<std::string>());
  }
  if (hasPatchFile && hash.empty())
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: patches/" + v.taskId +
                      ".patch exists but gitDiffHash is empty — the vector "
                      "MUST bind to the patch (E.2.3 bindPatchHash); "
                      "recovery: leaf resubmits with the short hash");
  if (!hasPatchFile && !hash.empty())
    return reject("VECTOR_FIELD_INVALID",
                  "result vector: gitDiffHash \"" + hash +
                      "\" supplied but no patch file exists — expected \"\" "
                      "with the no-patch reason in summary (E.2.3)");
  v.gitDiffHash = hash;

  // exportedState: object of scalars only, <= 4 KiB serialized.
  // Oversize OR non-scalar => BUS_VALUE_TOO_LARGE — REFUSE, never
  // truncate (spec E.2.3: truncate-with-flag considered and REJECTED).
  if (!in.contains("exportedState")) {
    v.exportedState = nlohmann::json::object();  // required, {} allowed
  } else if (!in["exportedState"].is_object()) {
    return reject("BUS_VALUE_TOO_LARGE",
                  "result vector: exportedState must be an object of "
                  "string/number/boolean scalars (E.2.3) — got " +
                      std::string(in["exportedState"].type_name()) +
                      "; REFUSED, vector unchanged; recovery: leaf resubmits "
                      "narrower (no truncation, no flag bit)");
  } else {
    for (auto it = in["exportedState"].begin(); it != in["exportedState"].end();
         ++it) {
      const auto& val = it.value();
      if (!val.is_string() && !val.is_number() && !val.is_boolean())
        return reject(
            "BUS_VALUE_TOO_LARGE",
            "result vector: exportedState[\"" + it.key() + "\"] is " +
                std::string(val.type_name()) +
                " — values are strings/numbers/booleans ONLY (E.2.3); "
                "REFUSED with BUS_VALUE_TOO_LARGE, vector unchanged; "
                "recovery: leaf resubmits narrower (no truncation)");
    }
    const std::string ser = in["exportedState"].dump();
    if (static_cast<long>(ser.size()) > kExportedStateMaxBytes)
      return reject("BUS_VALUE_TOO_LARGE",
                    "result vector: exportedState serializes to " +
                        std::to_string(ser.size()) + " bytes > " +
                        std::to_string(kExportedStateMaxBytes) +
                        " (E.2.3 cap); REFUSED, vector unchanged; recovery: "
                        "leaf resubmits narrower — host NEVER truncates");
    v.exportedState = in["exportedState"];
  }

  out.ok = true;
  out.code = "";
  out.message = "";
  return out;
}

nlohmann::json resultVectorJson(const ResultVector& v) {
  nlohmann::json j;
  j["taskId"] = v.taskId;
  j["status"] = v.status;
  j["summary"] = v.summary;
  j["changedFiles"] = v.changedFiles;
  j["gitDiffHash"] = v.gitDiffHash;
  j["exportedState"] = v.exportedState;
  return j;
}

}  // namespace dshlite
