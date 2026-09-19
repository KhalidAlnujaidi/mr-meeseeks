// usage_probe.cpp — P1: safe `.coli_usage` reader. See usage_probe.hpp.

#include "dshlite/usage_probe.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace dshlite {
namespace {
// inkling's binary format magic (route_trace.h RT_IKU1_MAGIC): a file
// starting with these 4 bytes is another engine's ranking — refused by
// name, exactly like the engines do, never parsed as text.
constexpr uint32_t kIku1Magic = 0x31554B49u;  // "IKU1" little-endian
}  // namespace

uint32_t coliEngineHash(const char* name) {
  uint32_t h = 2166136261u;  // FNV-1a offset basis (route_trace.h rt_hash)
  for (const char* p = name; p && *p; ++p) {
    h ^= static_cast<unsigned char>(*p);
    h *= 16777619u;
  }
  return h;
}

UsageProbe probeUsageFile(const std::string& path, long long freshWithinMs) {
  UsageProbe p;
  p.path = path;

  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    p.error = (errno == ENOENT) ? "no such file (engine has not run a turn yet?)"
                                : std::string("stat failed: ") + std::strerror(errno);
    return p;
  }
  p.exists = true;
  p.mtimeMs = static_cast<long long>(st.st_mtime) * 1000;

  // F37: no locking needed — the engine publishes via temp+rename, so
  // this open() sees a complete old or new file. Read the whole thing;
  // it is sparse text (one line per hot expert), bounded in practice by
  // layers x experts, and a torn read degrades to parse failure below.
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    p.error = std::string("open failed: ") + std::strerror(errno);
    return p;
  }
  std::string data;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
  std::fclose(f);

  if (data.size() >= 4 &&
      std::memcmp(data.data(), &kIku1Magic, 4) == 0) {
    p.error = "IKU1 binary history (inkling engine) — refused by magic, "
              "not parsed as text (matches the engines' own rule)";
    return p;
  }

  // Line-wise parse: fscanf-style triples. Header records carry a
  // NEGATIVE layer (-1 dims, -2 version+engine hash); data records have
  // layer >= 0. Malformed line => stop parsing, keep what was read
  // (old readers do exactly this — route_trace.h format note).
  size_t pos = 0;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    const std::string line =
        data.substr(pos, (nl == std::string::npos ? data.size() : nl) - pos);
    pos = (nl == std::string::npos) ? data.size() : nl + 1;
    // Header records first (negative layer), then data triples.
    int a = 0, b = 0;
    unsigned c = 0;
    if (std::sscanf(line.c_str(), "-1 %d %d", &a, &b) == 2) {
      p.nLayers = a;
      p.nExperts = b;
      continue;
    }
    if (std::sscanf(line.c_str(), "-2 %d %u", &a, &c) == 2) {
      p.formatVersion = a;
      p.engineIdHash = c;
      continue;
    }
    int l = 0, e = 0;
    unsigned cnt = 0;
    if (std::sscanf(line.c_str(), "%d %d %u", &l, &e, &cnt) == 3 && l >= 0) {
      ++p.records;
      p.totalHits += cnt;
      continue;
    }
    if (!line.empty()) {
      // Unparseable line: stop (old-reader semantics), keep prior data.
      p.error = "stopped at malformed line: " + line.substr(0, 80);
      break;
    }
  }

  p.ok = (p.records > 0) || (p.nLayers >= 0) || (p.formatVersion >= 0);
  if (!p.ok && p.error.empty()) p.error = "empty or unrecognized file";

  // F38 warm: cumulative history is only 'warm' evidence if the engine
  // touched it recently (it rewrites after every turn).
  if (p.ok && p.mtimeMs > 0) {
    const long long nowMs = static_cast<long long>(std::time(nullptr)) * 1000;
    p.warm = (nowMs - p.mtimeMs) <= freshWithinMs && (nowMs - p.mtimeMs) >= -1000;
  }
  return p;
}

}  // namespace dshlite
