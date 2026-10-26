#pragma once

#include <cstddef>
#include <string>

// The text-utf8 tool owns one invariant: every string that leaves this module
// is valid UTF-8, and a cut never lands inside a code point.
//
// It is intentionally dependency-free and knows nothing about actions,
// prompts, or policy. The action layer uses it to produce safe tool output;
// the AI layer uses it for prompt excerpts; the JSON boundary uses sanitize()
// as the last line of defense.
namespace text_utf8 {

// U+FFFD REPLACEMENT CHARACTER.
inline constexpr const char* kReplacement = "\xEF\xBF\xBD";

// True when `s` is well-formed UTF-8 (no overlongs, surrogates, or values
// above U+10FFFF).
bool valid(const std::string& s);

// Returns `s` with every invalid byte sequence replaced by U+FFFD. Valid input
// is returned unchanged. `repaired` (optional) receives the number of
// replacement characters inserted.
std::string sanitize(const std::string& s, size_t* repaired = nullptr);

// Largest prefix length <= maxBytes that ends on a code-point boundary.
// Operates on the raw bytes; call sanitize() first when validity matters.
size_t headCut(const std::string& s, size_t maxBytes);

// Start index of the largest suffix <= maxBytes that begins on a code-point
// boundary. Operates on the raw bytes.
size_t tailCut(const std::string& s, size_t maxBytes);

// Sanitized, boundary-safe prefix/suffix.
std::string head(const std::string& s, size_t maxBytes);
std::string tail(const std::string& s, size_t maxBytes);

// Like tail(), but when the retained window contains a newline, start just
// after the first newline in that window. Never returns a partial code point.
std::string tailLines(const std::string& s, size_t maxBytes);

} // namespace text_utf8
