#include "aichat/io/json.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ai-tools/text-utf8/text-utf8.h"

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Raw characters between quotes, starting at (and excluding) the opening quote.
std::string scanJsonString(const std::string& s, size_t open) {
    std::string raw;
    for (size_t i = open + 1; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            raw += s[i++];
            raw += s[i];
            continue;
        }
        if (s[i] == '"') break;
        raw += s[i];
    }
    return raw;
}

} // namespace

std::string jsonEscape(const std::string& s, bool quiet) {
    // Defense in depth: by the time a string reaches the wire it must be valid
    // UTF-8. Callers should never cut text mid-character, but if one does,
    // replace the damage instead of sending a body the server will reject.
    size_t repaired = 0;
    const std::string clean = text_utf8::sanitize(s, &repaired);
    if (repaired > 0 && !quiet) {
        std::fprintf(stderr,
                     "ai-chat: replaced %zu invalid UTF-8 sequence(s) in an "
                     "outgoing request\n",
                     repaired);
    }

    std::string o;
    o.reserve(clean.size() + 8);
    for (unsigned char ch : clean) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", ch);
                    o += buf;
                } else {
                    o += static_cast<char>(ch);
                }
        }
    }
    return o;
}

std::string jsonUnescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') { o += s[i]; continue; }
        if (++i >= s.size()) break;
        switch (s[i]) {
            case 'n':  o += '\n'; break;
            case 't':  o += '\t'; break;
            case 'r':  o += '\r'; break;
            case 'b':  o += '\b'; break;
            case 'f':  o += '\f'; break;
            case '/':  o += '/';  break;
            case '"':  o += '"';  break;
            case '\\': o += '\\'; break;
            case 'u': {
                unsigned cp = 0;
                bool ok = i + 4 < s.size();
                for (int k = 1; k <= 4 && ok; ++k) {
                    int h = hexVal(s[i + k]);
                    if (h < 0) { ok = false; break; }
                    cp = (cp << 4) | static_cast<unsigned>(h);
                }
                if (!ok) break;
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() &&
                    s[i + 1] == '\\' && s[i + 2] == 'u') { // surrogate pair
                    unsigned lo = 0;
                    bool okLo = true;
                    for (int k = 3; k <= 6; ++k) {
                        int h = hexVal(s[i + k]);
                        if (h < 0) { okLo = false; break; }
                        lo = (lo << 4) | static_cast<unsigned>(h);
                    }
                    if (okLo && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                // An unpaired surrogate cannot be encoded as UTF-8; the
                // server would reject it. Use the replacement character.
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                appendUtf8(o, cp);
                break;
            }
            default:
                o += s[i]; // unknown escape: take it literally
        }
    }
    return o;
}

bool extractDelta(const std::string& json, std::string& out) {
    size_t d = json.find("\"delta\"");
    if (d == std::string::npos) return false;
    size_t k = json.find("\"content\":", d);
    if (k == std::string::npos) return false;
    k += strlen("\"content\":");
    while (k < json.size() && (json[k] == ' ' || json[k] == '\t')) ++k;
    if (k >= json.size() || json[k] != '"') return false; // e.g. "content":null
    out = jsonUnescape(scanJsonString(json, k));
    return true;
}

bool extractUsage(const std::string& json, long long& promptTokens,
                  long long& completionTokens, long long& totalTokens) {
    const size_t usage = json.find("\"usage\"");
    if (usage == std::string::npos) return false;

    auto number = [&](const char* key, long long& out) -> bool {
        const std::string needle = std::string("\"") + key + "\":";
        size_t p = json.find(needle, usage);
        if (p == std::string::npos) return false;
        p += needle.size();
        while (p < json.size() &&
               std::isspace(static_cast<unsigned char>(json[p])))
            ++p;
        if (p >= json.size() || !std::isdigit(static_cast<unsigned char>(json[p])))
            return false;
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(json.c_str() + p, &end, 10);
        if (errno != 0 || end == json.c_str() + p || value < 0) return false;
        out = value;
        return true;
    };

    bool any = false;
    any |= number("prompt_tokens", promptTokens);
    any |= number("completion_tokens", completionTokens);
    any |= number("total_tokens", totalTokens);
    if (!any) return false;
    if (totalTokens <= 0) totalTokens = promptTokens + completionTokens;
    return true;
}
