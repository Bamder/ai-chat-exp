#include "aichat/core/facts/text.h"

#include "ai-tools/text-utf8/text-utf8.h"

namespace facts {

std::string trimText(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' ||
                     s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' ||
                     s[b - 1] == '\n' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

std::string compact(const std::string& s, size_t max) {
    std::string t;
    t.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\n' || c == '\r')
            t += " | ";
        else if (c == '\t')
            t += ' ';
        else
            t += c;
    }

    std::string out;
    bool space = false;
    for (char c : t) {
        if (c == ' ') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();

    // Blank lines become two separators; fold them into one.
    while (out.find("| |") != std::string::npos)
        out.replace(out.find("| |"), 3, "|");

    if (out.size() > max) out = text_utf8::head(out, max) + "…";
    return out;
}

std::string firstLine(const std::string& s) {
    const size_t e = s.find('\n');
    return compact(e == std::string::npos ? s : s.substr(0, e));
}

std::string field(const std::string& text, const std::string& label) {
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p);
        if (line.rfind(label, 0) == 0)
            return compact(line.substr(label.size()), 400);
        if (e == text.size()) break;
        p = e + 1;
    }
    return "";
}

std::string afterRule(const std::string& s) {
    const size_t p = s.find("\n---\n");
    if (p == std::string::npos) return s;
    return s.substr(p + 5);
}

std::string escapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

std::string unescapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const char n = s[++i];
        if (n == 't')
            out += '\t';
        else if (n == 'n')
            out += '\n';
        else
            out += n;
    }
    return out;
}

} // namespace facts
