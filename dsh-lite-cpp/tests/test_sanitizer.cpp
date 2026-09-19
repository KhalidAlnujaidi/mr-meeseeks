// test_sanitizer.cpp — Milestone 1 acceptance: binary blocks, broken
// multi-byte sequences, and terminal colors are cleaned; truncation keeps
// head 2048 + marker + tail 1024 past 4096 chars. No third-party test
// framework: assert-style checks, non-zero exit on failure.

#include <cassert>
#include <iostream>
#include <string>

#include "dshlite/sanitizer.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++failures;
}
}  // namespace

int main() {
  using dshlite::sanitize;

  // 1. Plain text + allowed whitespace pass through untouched.
  check(sanitize("hello world\n\tline2\r\n") == "hello world\n\tline2\r\n",
        "printable + tab/LF/CR preserved");

  // 2. Mixed binary block: NUL, C0 controls, DEL, high bytes dropped.
  {
    std::string raw = std::string("ab\x00\x01\x07\x08\x0B\x0C\x7F\xFF", 10) + "cd";
    check(sanitize(raw) == "abcd", "binary block cleaned to abcd");
  }

  // 3. Broken multi-byte UTF-8: lone continuation + truncated 3-byte seq.
  {
    std::string raw = "A\x80\xE2\x82" "B";  // 0x80 stray, E2 82 cut off
    check(sanitize(raw) == "AB", "broken multibyte dropped, ASCII kept");
  }
  // 4. Valid UTF-8 is NOT preserved (non-printable-ASCII policy): dropped.
  {
    std::string e = "\xC3\xA9";  // U+00E9 e-acute
    check(sanitize(e).empty(), "non-ASCII bytes dropped per SRS");
  }

  // 5. ANSI colors stripped: ESC [ 31 m ... ESC [ 0 m.
  {
    std::string raw = "\x1B[31mred\x1B[0m plain \x1B[1;32mbold-green\x1B[m!";
    check(sanitize(raw) == "red plain bold-green!", "SGR colors stripped");
  }

  // 6. Cursor movement + erase-line stripped.
  {
    std::string raw = "a\x1B[2Kb\x1B[1Ac\x1B[?25l";
    check(sanitize(raw) == "abc", "cursor/erase CSI stripped");
  }

  // 7. OSC hyperlink (BEL-terminated) stripped.
  {
    std::string raw = "\x1B]8;;http://example.com\x07link\x1B]8;;\x07";
    check(sanitize(raw) == "link", "OSC hyperlink stripped");
  }

  // 8. Lone ESC and ESC + single char dropped, no hang.
  {
    std::string raw = "x\x1B";
    check(sanitize(raw) == "x", "lone trailing ESC dropped");
    check(sanitize(std::string("x\x1BM" "y")) == "xy", "ESC+M dropped");
    // ESC ( B is a 3-byte charset designation: all three consumed.
    check(sanitize(std::string("(\x1B(B0")) == "(0", "charset select dropped");
  }

  // 9. Truncation: 5000 x's -> 2048 + marker + 1024.
  {
    std::string big(5000, 'x');
    std::string got = sanitize(big);
    std::string marker = dshlite::TRUNCATION_MARKER;
    std::size_t expect = 2048 + marker.size() + 1024;
    bool shape = got.size() == expect &&
                 got.substr(0, 2048) == std::string(2048, 'x') &&
                 got.substr(2048, marker.size()) == marker &&
                 got.substr(2048 + marker.size()) == std::string(1024, 'x');
    check(shape, "truncation head+marker+tail shape");
  }

  // 10. Head/tail content preserved across truncation (not just fill).
  {
    std::string big;
    for (int i = 0; i < 5000; ++i) big.push_back('A' + (i % 26));
    std::string got = sanitize(big);
    check(got.substr(0, 5) == big.substr(0, 5) &&
              got.substr(got.size() - 5) == big.substr(big.size() - 5),
          "truncation keeps true head and tail");
  }

  // 11. Boundary: exactly 4096 chars passes through uncut.
  {
    std::string edge(4096, 'q');
    check(sanitize(edge) == edge, "4096-char output uncut");
    check(sanitize(edge + "q").find("TRUNCATED") != std::string::npos,
          "4097-char output truncated");
  }

  // 12. Empty input.
  check(sanitize("").empty(), "empty in, empty out");

  std::cout << (failures == 0 ? "SANITIZER PASS\n" : "SANITIZER FAIL\n");
  return failures == 0 ? 0 : 1;
}
