// sanitizer.cpp — Module 1: Context Firebreak & Bitstream Sanitizer.
//
// Single-pass O(n) state machine, no regex, RAII only.

#include "dshlite/sanitizer.hpp"

#include <string>

namespace dshlite {
namespace {

constexpr char ESC = '\x1B';

bool isCsiFinal(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return u >= 0x40 && u <= 0x7E;
}

}  // namespace

std::string sanitize(std::string_view in) {
  const char* p = in.data();
  const std::size_t n = in.size();
  // Upper bound: cleaned output never exceeds input length. Fill via raw
  // pointer (no push_back overhead), shrink at the end. Bulk memcpy for
  // runs of clean bytes keeps the 10 MB / 15 ms budget with wide margin.
  std::string out;
  out.resize(n);
  char* w = out.data();

  std::size_t i = 0;
  while (i < n) {
    // Fast path: span of printable ASCII + TAB/LF/CR.
    std::size_t j = i;
    while (j < n) {
      unsigned char d = static_cast<unsigned char>(p[j]);
      if ((d >= 0x20 && d <= 0x7E) || d == '\t' || d == '\n' || d == '\r')
        ++j;
      else
        break;
    }
    if (j > i) {
      std::size_t span = j - i;
      __builtin_memcpy(w, p + i, span);
      w += span;
      i = j;
      if (i >= n) break;
    }

    unsigned char c = static_cast<unsigned char>(p[i]);
    if (c == static_cast<unsigned char>(ESC)) {
      // ANSI escape: consume the whole sequence, emit nothing.
      if (i + 1 >= n) break;  // lone trailing ESC: drop
      char next = p[i + 1];
      if (next == '[') {
        // CSI: ESC [ params... intermediates... final(0x40-0x7E)
        i += 2;
        while (i < n && !isCsiFinal(p[i])) ++i;
        if (i < n) ++i;  // consume the final byte
      } else if (next == ']') {
        // OSC: ESC ] ... terminated by BEL or ESC backslash (ST)
        i += 2;
        while (i < n) {
          if (p[i] == '\x07') {
            ++i;
            break;
          }
          if (p[i] == ESC && i + 1 < n && p[i + 1] == '\\') {
            i += 2;
            break;
          }
          ++i;
        }
      } else if (next == 'P' || next == 'X' || next == '^' ||
                 next == '_') {
        // DCS/SOS/PM/APC: consume until ST (ESC \) or BEL
        i += 2;
        while (i < n) {
          if (p[i] == '\x07') {
            ++i;
            break;
          }
          if (p[i] == ESC && i + 1 < n && p[i + 1] == '\\') {
            i += 2;
            break;
          }
          ++i;
        }
      } else if (next == '(' || next == ')' || next == '#' ||
                 next == '%') {
        // Charset select / single-shift: ESC + introducer + 1 char
        i += 3;
      } else {
        // Any other ESC + single char (e.g. M, 7, 8, c, =, >): skip both
        i += 2;
      }
      continue;
    }
    // Drop: NUL, other C0 controls, DEL, all bytes >= 0x80 (kills broken
    // multi-byte UTF-8 rather than poisoning Brain context).
    ++i;
  }

  out.resize(static_cast<std::size_t>(w - out.data()));

  if (out.size() > SANITIZE_TRUNCATE_LIMIT) {
    std::string cut;
    cut.reserve(SANITIZE_HEAD + 128 + SANITIZE_TAIL);
    cut.append(out.data(), SANITIZE_HEAD);
    cut.append(TRUNCATION_MARKER);
    cut.append(out.data() + out.size() - SANITIZE_TAIL, SANITIZE_TAIL);
    return cut;
  }
  return out;
}

}  // namespace dshlite
