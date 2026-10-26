#include "ai-tools/text-utf8/text-utf8.h"

#include <algorithm>

namespace text_utf8 {

namespace {

constexpr unsigned char kRepl[] = {0xEF, 0xBF, 0xBD};

bool continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// Length of the valid sequence starting at `i`, or 0 when it is invalid or
// truncated. Rejects overlongs, UTF-16 surrogates, and values > U+10FFFF.
size_t seqLen(const std::string& s, size_t i) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) return 1;
    if (b < 0xC2) return 0;

    if (b < 0xE0) { // 2 bytes
        if (i + 1 >= s.size()) return 0;
        if (!continuation(static_cast<unsigned char>(s[i + 1]))) return 0;
        return 2;
    }
    if (b < 0xF0) { // 3 bytes
        if (i + 2 >= s.size()) return 0;
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if (!continuation(b1) || !continuation(b2)) return 0;
        if (b == 0xE0 && b1 < 0xA0) return 0;          // overlong
        if (b == 0xED && b1 >= 0xA0) return 0;         // surrogate
        return 3;
    }
    if (b < 0xF5) { // 4 bytes
        if (i + 3 >= s.size()) return 0;
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
        if (!continuation(b1) || !continuation(b2) || !continuation(b3))
            return 0;
        if (b == 0xF0 && b1 < 0x90) return 0;          // overlong
        if (b == 0xF4 && b1 >= 0x90) return 0;         // > U+10FFFF
        return 4;
    }
    return 0;
}

// Number of bytes a malformed sequence occupies: the lead byte plus any
// continuation bytes that belong to its expected length (the "maximal
// subpart"). This turns one truncated character into one U+FFFD, not two.
size_t badRun(const std::string& s, size_t i) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    size_t expected = 1;
    if (b >= 0xC2 && b <= 0xDF) expected = 2;
    else if (b >= 0xE0 && b <= 0xEF) expected = 3;
    else if (b >= 0xF0 && b <= 0xF4) expected = 4;

    size_t n = 1;
    while (n < expected && i + n < s.size() &&
           continuation(static_cast<unsigned char>(s[i + n])))
        ++n;
    return n;
}

} // namespace

bool valid(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const size_t n = seqLen(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

std::string sanitize(const std::string& s, size_t* repaired) {
    size_t count = 0;
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const size_t n = seqLen(s, i);
        if (n > 0) {
            out.append(s, i, n);
            i += n;
            continue;
        }
        out.append(reinterpret_cast<const char*>(kRepl), sizeof kRepl);
        i += badRun(s, i);
        ++count;
    }
    if (repaired) *repaired = count;
    return out;
}

size_t headCut(const std::string& s, size_t maxBytes) {
    if (maxBytes >= s.size()) return s.size();
    size_t n = maxBytes;
    while (n > 0 && continuation(static_cast<unsigned char>(s[n]))) --n;
    return n;
}

size_t tailCut(const std::string& s, size_t maxBytes) {
    if (maxBytes >= s.size()) return 0;
    size_t n = s.size() - maxBytes;
    while (n < s.size() && continuation(static_cast<unsigned char>(s[n]))) ++n;
    return n;
}

std::string head(const std::string& s, size_t maxBytes) {
    const std::string clean = sanitize(s);
    if (clean.size() <= maxBytes) return clean;
    return clean.substr(0, headCut(clean, maxBytes));
}

std::string tail(const std::string& s, size_t maxBytes) {
    const std::string clean = sanitize(s);
    if (clean.size() <= maxBytes) return clean;
    return clean.substr(tailCut(clean, maxBytes));
}

std::string tailLines(const std::string& s, size_t maxBytes) {
    const std::string clean = sanitize(s);
    if (clean.size() <= maxBytes) return clean;

    size_t n = tailCut(clean, maxBytes);
    const size_t nl = clean.find('\n', n);
    if (nl != std::string::npos && nl + 1 < clean.size()) n = nl + 1;
    return clean.substr(n);
}

} // namespace text_utf8
