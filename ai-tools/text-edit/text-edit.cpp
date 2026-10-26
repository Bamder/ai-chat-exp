#include "ai-tools/text-edit/text-edit.h"

#include <algorithm>

namespace text_edit {

namespace {

size_t lineOf(const std::string& s, size_t offset) {
    const size_t line = static_cast<size_t>(
        std::count(s.begin(), s.begin() + static_cast<long>(offset), '\n'));
    return line + 1;
}

std::string countText(size_t n) {
    return std::to_string(n) + (n == 1 ? " time" : " times");
}

} // namespace

Result replaceOnce(const std::string& source, const std::string& oldText,
                   const std::string& newText) {
    Result r;
    if (oldText.empty()) {
        r.ok = false;
        r.error = "the old text is empty; nothing to locate";
        return r;
    }

    const size_t at = source.find(oldText);
    if (at == std::string::npos) {
        r.ok = false;
        r.error = "the old text does not occur in the file";
        return r;
    }

    // A second occurrence makes the site ambiguous. Report the count so the
    // caller knows to add surrounding context rather than retry blindly.
    size_t occurrences = 1;
    for (size_t p = at + oldText.size();
         (p = source.find(oldText, p)) != std::string::npos;
         p += oldText.size())
        ++occurrences;
    if (occurrences > 1) {
        r.ok = false;
        r.occurrences = occurrences;
        r.error = "the old text appears " + countText(occurrences) +
                  " (more than once); add surrounding context so exactly one "
                  "site matches";
        return r;
    }

    r.occurrences = 1;
    r.offset = at;
    r.line = lineOf(source, at);
    r.text.reserve(source.size() - oldText.size() + newText.size());
    r.text.append(source, 0, at);
    r.text += newText;
    r.text.append(source, at + oldText.size(), std::string::npos);
    return r;
}

} // namespace text_edit
