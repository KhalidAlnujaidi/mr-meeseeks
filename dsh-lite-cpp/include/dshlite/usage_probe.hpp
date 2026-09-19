#pragma once
// usage_probe.hpp — P1 (roadmap G3.3/F5/F37/F38): safe cross-process
// reader for Colibri's `.coli_usage` expert-heat history.
//
// Format (authoritative source: colibri c/route_trace.h, RT_FORMAT_VERSION 1):
//   Text, one sparse record per line: "<layer> <expert> <count>"
//   Header records (negative layer, old readers skip them):
//     -1 <n_layers> <n_experts>
//     -2 <format_version> <engine_id_hash>   (FNV-1a 32 of the engine name)
//   A legacy headerless variant (bare triples) is also accepted.
//
// Race safety (F37): the engine writes via temp file + atomic rename(),
// so a reader observes either the previous or the new file — never a
// partial one. This reader therefore needs NO locking: it opens, reads
// fully, and TOLERATES every failure mode as "no data" (never throws,
// never blocks). Per F5/F38 the result is best-effort telemetry ONLY —
// nothing here may gate execution until the roadmap's A/B says so.
//
// Heat semantics (F38): the file is CUMULATIVE across sessions, not a
// live snapshot. `warm` therefore means: the file exists, parses, AND
// its mtime is within `freshWithinMs` of now (the engine appends/renames
// every turn — a stale mtime means the engine is idle or dead).

#include <cstdint>
#include <string>

namespace dshlite {

/// Parsed `.coli_usage` snapshot. All fields are best-effort (F5).
struct UsageProbe {
  bool ok = false;            ///< parsed at least the records (or headers)
  bool exists = false;        ///< file present at probe time
  std::string path;
  int nLayers = -1;           ///< from the -1 header record (-1 = absent)
  int nExperts = -1;
  int formatVersion = -1;     ///< from the -2 record (-1 = legacy/absent)
  uint32_t engineIdHash = 0;  ///< FNV-1a of the writing engine's name
  long records = 0;           ///< data triples parsed (layer >= 0)
  unsigned long long totalHits = 0;  ///< sum of counts (cumulative!)
  long long mtimeMs = 0;      ///< file mtime, ms epoch (0 = unknown)
  bool warm = false;          ///< ok && mtime within freshWithinMs (F38)
  std::string error;          ///< non-fatal note when !ok (never throws)
};

/// Read + parse one snapshot. NEVER throws: unreadable, truncated (torn
/// rename on a non-atomic FS), binary (IKU1 inkling format — recognized
/// and refused by magic, matching the engine's own rule), or empty files
/// all return {ok:false} with `error` set. `freshWithinMs` defaults to
/// 120 s (~ two stall intervals at the roadmap default).
UsageProbe probeUsageFile(const std::string& path, long long freshWithinMs = 120000);

/// FNV-1a 32 — same hash the engine writes into the -2 record
/// (route_trace.h rt_hash), exposed so callers can map engine names
/// ("olmoe", "glm53", ...) to the recorded id without re-implementing.
uint32_t coliEngineHash(const char* name);

}  // namespace dshlite
