// bench_sanitizer.cpp — SRS perf gate: 10 MB of terminal-colored output
// must sanitize in under 15 ms. Fails (exit 1) past budget.
//
// The budget is a PRODUCTION-build wall-clock law. Under ASan/UBSan the
// same code runs ~3x slower from instrumentation, which measures the
// sanitizer, not the code — so this gate SKIPs (loudly, exit 0) when
// built with sanitizers rather than reporting a false failure or a
// faked pass.

#include <chrono>
#include <iostream>
#include <string>

#include "dshlite/sanitizer.hpp"

namespace {
bool builtWithSanitizers() {
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
  return true;
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
  return true;
#endif
  return false;
}
}  // namespace

int main() {
  if (builtWithSanitizers()) {
    std::cout << "SKIPPED: perf gate is a production-build law; this binary "
                 "is ASan/UBSan-instrumented (~3x overhead would measure the "
                 "sanitizer, not the code)\n";
    return 0;
  }
  // Build ~10 MB with realistic grime: colors, cursor moves, binary junk.
  std::string raw;
  raw.reserve(10 << 20);
  const std::string chunk =
      "\x1B[31mCompiling foo.cpp\x1B[0m ok\n\x1B[2K\x1B[1A\x00\x01\xFF";
  while (raw.size() < (10u << 20)) raw += chunk;

  std::string clean = dshlite::sanitize(raw);  // warmup (page in, steady clocks)
  long ms = 86400000L;
  for (int k = 0; k < 3; ++k) {
    auto a = std::chrono::steady_clock::now();
    clean = dshlite::sanitize(raw);
    auto b = std::chrono::steady_clock::now();
    long m =
        std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    if (m < ms) ms = m;
  }
  double mbs = raw.size() / 1048576.0;

  std::cout << "sanitized " << mbs << " MB in " << ms << " ms (budget 15 ms), "
            << "out=" << clean.size() << " chars\n";
  if (ms >= 15) {
    std::cout << "PERF FAIL\n";
    return 1;
  }
  std::cout << "PERF PASS\n";
  return 0;
}
