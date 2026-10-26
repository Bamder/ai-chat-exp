#include "ai-tools/text-grep/text-grep.h"

#include <algorithm>

#include "ai-tools/text-utf8/text-utf8.h"

namespace text_grep {

namespace {

// Byte offsets of every line start, so a match can be located in O(log n)
// instead of rescanning the buffer from the beginning.
std::vector<size_t> lineStarts(const std::string& s) {
    std::vector<size_t> starts;
    starts.push_back(0);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') starts.push_back(i + 1);
    }
    return starts;
}

std::string lineAt(const std::string& s, const std::vector<size_t>& starts,
                   const Match& m) {
    const auto it = std::upper_bound(starts.begin(), starts.end(), m.offset);
    const size_t index =
        it == starts.begin() ? 0 : static_cast<size_t>(it - starts.begin()) - 1;
    const size_t begin = starts[index];
    size_t end = index + 1 < starts.size() ? starts[index + 1] : s.size();
    while (end > begin && (s[end - 1] == '\n' || s[end - 1] == '\r')) --end;
    std::string line = s.substr(begin, end - begin);
    if (line.size() > kMaxLine) line.resize(text_utf8::headCut(line, kMaxLine));
    return line;
}

} // namespace

Result find(const std::string& haystack, const std::string& needle,
            size_t limit) {
    Result r;
    if (needle.empty()) {
        r.ok = false;
        r.error = "the search pattern is empty";
        return r;
    }

    const std::vector<size_t> starts = lineStarts(haystack);
    size_t from = 0;
    while (from <= haystack.size()) {
        const size_t at = haystack.find(needle, from);
        if (at == std::string::npos) break;
        ++r.total;
        if (limit == 0 || r.matches.size() < limit) {
            Match m;
            m.offset = at;
            m.line = static_cast<size_t>(
                         std::upper_bound(starts.begin(), starts.end(), at) -
                         starts.begin());
            m.text = lineAt(haystack, starts, m);
            r.matches.push_back(std::move(m));
        }
        from = at + needle.size();
    }
    return r;
}

} // namespace text_grep
