#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The text-grep tool locates a literal string inside a UTF-8 text buffer and
// reports where it is. It knows nothing about files, prompts, or policy: the
// caller supplies the bytes, so the same scan serves read-back checks,
// coverage claims, and the grep_file action.
//
// Literal matching is deliberate. The things this tool is used for — page
// markers, offsets, titles, paths — are exact strings, and a regex engine
// would add escaping mistakes without adding reach.
namespace text_grep {

struct Match {
    size_t line = 0;    // 1-based line number of the match
    size_t offset = 0;  // byte offset of the first byte of the match
    std::string text;   // the whole line, cut at a code-point boundary
};

struct Result {
    bool ok = true;
    std::string error;
    std::vector<Match> matches;  // at most `limit` entries
    size_t total = 0;            // matches found, including unlisted ones
};

// Finds every non-overlapping occurrence of `needle`. An empty needle is an
// error. `limit == 0` means no cap; each reported line is cut to `kMaxLine`
// bytes so one long line cannot flood a tool result.
Result find(const std::string& haystack, const std::string& needle,
            size_t limit);

inline constexpr size_t kMaxLine = 400;

} // namespace text_grep
