#include "ai-tools/sse-watch/sse-watch.h"

#include <cctype>
#include <cstring>

namespace sse_watch {

namespace {

std::string trimCopy(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Finds every `"key" : "` and reports whether any value is a non-empty
// string. This is a classifier, not a parser: the value is still decoded by
// the chat layer. It only has to answer "did this frame carry text?".
bool hasNonEmptyString(const std::string& s, const char* key) {
    const size_t keyLen = std::strlen(key);
    size_t from = 0;
    while ((from = s.find(key, from)) != std::string::npos) {
        size_t i = from + keyLen; // key is `"content"` including quotes
        from = i;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size() || s[i] != ':') continue;
        ++i;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size() || s[i] != '"') continue; // null or a number
        if (i + 1 < s.size() && s[i + 1] != '"') return true;
    }
    return false;
}

} // namespace

bool isTokenFrame(const std::string& line) {
    const std::string t = trimCopy(line);
    if (t.rfind("data:", 0) != 0) return false;
    const std::string data = trimCopy(t.substr(5));
    if (data == "[DONE]") return false;
    return hasNonEmptyString(data, "\"content\"") ||
           hasNonEmptyString(data, "\"reasoning_content\"") ||
           hasNonEmptyString(data, "\"reasoning\"");
}

bool stalled(long long nowMs, long long lastMs, bool sawToken,
             long long firstTokenMs, long long gapMs) {
    const long long limit = sawToken ? gapMs : firstTokenMs;
    if (limit <= 0) return false;
    return nowMs - lastMs > limit;
}

} // namespace sse_watch
