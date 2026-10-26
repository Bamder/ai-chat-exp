#pragma once

#include <cstddef>
#include <string>

// The text-edit tool performs a surgical replacement inside a UTF-8 text
// buffer. It exists because a full-file rewrite is not an edit: it forces the
// caller to reproduce every byte of the file from memory, and one misrecalled
// character corrupts the document.
//
// The rule is deliberately strict: the old text must occur exactly once.
// Zero occurrences means the target is gone; more than one means the caller
// has not supplied enough context to say which site it means. Both are
// errors, because guessing is worse than failing.
namespace text_edit {

struct Result {
    bool ok = true;
    std::string error;
    std::string text;          // full content after the replacement
    size_t offset = 0;         // byte offset where the old text was replaced
    size_t line = 0;           // 1-based line of that offset
    size_t occurrences = 0;    // how many times `old` occurred
};

// Replaces the single occurrence of `oldText` with `newText`.
// `oldText` must be non-empty. `newText` may be empty (a deletion).
Result replaceOnce(const std::string& source, const std::string& oldText,
                   const std::string& newText);

} // namespace text_edit
