#pragma once
// postcondition.hpp — F72 promotion: artifact-aware verification (layer 2).
//
// HISTORY / WHY THIS EXISTS
// -------------------------
// F72 (docs/colibri-roadmap.md, bench/h2h/README.md) established that an
// exit-0 verify proves a command RAN, not that it did the TASK: the
// caught live case was a solicited `echo hello > hello.txt` — exit 0,
// artifact missing the required content. "verified" therefore requires
// BOTH layers:
//
//   layer 1  exit code            (verifyViaSpawn, G2.6/F9)
//   layer 2  content postcondition (THIS header)
//
// F72's fix landed bench-side (tests/h2h_ours.cpp, referee.py) while the
// runtime loop kept layer 1 only; tests/h2h_ours.cpp noted "a core
// postcondition hook is future work". This header is that promotion.
//
// PREDICATE VOCABULARY (P2-c)
// ---------------------------
// The set here is a SUPERSET of the original bench-side subset
// (equals/contains/exists) so the iso harness can migrate onto runtime
// predicates instead of keeping a second, disagreeing implementation.
// Conditions mirror bench/iso_harness/referee.py check_one():
//
//   {"file": F, "equals":   S}   exact match, trailing newlines trimmed
//   {"file": F, "contains": S}   substring present
//   {"file": F, "regex":    R}   ECMAScript regex, multiline
//   {"file": F, "exists":  BOOL} existence equals BOOL (negation is how
//                               T9/T10 canaries assert ABSENCE)
//   {"file": F}                  file-exists-only (legacy C++ behavior)
//   {"all_of": [cond, ...]}      conjunction of the above
//   null / {}                    no postcondition (caller decides policy)
//
// SAFETY LAWS
// -----------
// - Path traversal guard: the artifact must resolve INSIDE the workspace
//   (defense in depth; the spawner already pins the child CWD there).
//   A condition naming a path outside the workspace is a FAIL, never a
//   read of the host filesystem.
// - Evaluation is HOST-DECLARED and model-independent: the model never
//   supplies its own postcondition, so this layer stays sycophancy-immune
//   exactly like the exit-code layer (F9).
// - Reporting is honest: "no postcondition declared" is never laundered
//   into "content-verified" (F72 wording). See PostconditionOutcome.

#include <string>

#include <nlohmann/json.hpp>

namespace dshlite {

/// Outcome of one postcondition evaluation. `evaluated` distinguishes
/// "there was nothing to check" from "a check ran and passed" — the
/// difference the pre-promotion code could not express.
struct PostconditionResult {
  bool pass = false;       ///< predicate held (true when nothing to check)
  bool evaluated = false;  ///< false => no postcondition declared
  std::string detail;      ///< human-readable reason (sanitized upstream)
};

/// Evaluate a tasks.json-shaped ground_truth condition against the
/// artifacts in `workspaceDir`. Pure filesystem I/O, no network (F9).
///
/// Throws std::runtime_error only if `workspaceDir` cannot be resolved
/// (a caller bug); every condition failure is reported via the return
/// value, never by throwing.
PostconditionResult checkPostcondition(const std::string& workspaceDir,
                                       const nlohmann::json& gt);

}  // namespace dshlite
