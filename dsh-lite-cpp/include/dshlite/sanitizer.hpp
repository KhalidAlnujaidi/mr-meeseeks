#pragma once
// sanitizer.hpp — Module 1: Context Firebreak & Bitstream Sanitizer (SRS Milestone 1).
//
// Contract:
//  - Keep printable ASCII 0x20-0x7E plus TAB (0x09), LF (0x0A), CR (0x0D).
//  - Drop everything else (NUL, control chars, lone ESC, bytes >= 0x80
//    including broken multi-byte UTF-8 sequences).
//  - Strip ANSI escape sequences: CSI (ESC [ ... final), OSC (ESC ] ... BEL
//    or ESC \), charset selects (ESC ( X), and any other ESC + single char.
//  - Truncate: cleaned output > 4096 chars keeps head 2048 + marker + tail 1024.
//
// Performance: single pass, O(n), no regex. Must clear 10 MB in < 15 ms.
// RAII only: no raw new, output std::string freed at end of turn.

#include <cstddef>
#include <string>
#include <string_view>

namespace dshlite {

inline constexpr std::size_t SANITIZE_TRUNCATE_LIMIT = 4096;
inline constexpr std::size_t SANITIZE_HEAD = 2048;
inline constexpr std::size_t SANITIZE_TAIL = 1024;
inline constexpr const char* TRUNCATION_MARKER =
    "\n... [TRUNCATED RAW LOGS TO PROTECT BRAIN CONTEXT] ...\n";

/// Sanitize a raw worker byte stream into Brain-safe text.
std::string sanitize(std::string_view in);

}  // namespace dshlite
