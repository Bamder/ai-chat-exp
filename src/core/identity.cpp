#include "aichat/core/identity.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/facts/text.h"
#include "aichat/core/util.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace identity {

namespace {

constexpr size_t kRenderChars = 8000;

std::string today() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return s;
}

std::string oneLine(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ') {
            if (!space && !out.empty()) out += ' ';
            space = true;
        } else {
            out += c;
            space = false;
        }
    }
    return trim(out);
}

std::string sanitizeHeader(std::string s) {
    s = oneLine(std::move(s));
    for (char& c : s) {
        if (c == '|' || c == '[' || c == ']') c = '/';
    }
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// Names compare case-insensitively but keep their surface form.
std::string normalizeName(const std::string& text) {
    std::string s = lowerAscii(oneLine(text));
    std::string out;
    bool space = false;
    for (unsigned char c : s) {
        if (c >= 0x80) {
            out += static_cast<char>(c);
            space = false;
        } else if (std::isalnum(c) || c == '-' || c == '_') {
            out += static_cast<char>(c);
            space = false;
        } else if (c == ' ') {
            if (!out.empty() && !space) out += ' ';
            space = true;
        }
    }
    return trim(out);
}

// Login-ids keep punctuation such as '.' and '@' because those bytes are
// meaningful in a session label. Comparison is case-insensitive.
std::string loginKey(const std::string& text) {
    return lowerAscii(trim(text));
}

// Roles and relations are deduped by meaning, so "my father" and "dad" are
// the same identity. Only common relation synonyms are canonicalised; this is
// deliberately small and predictable.
std::string canonicalRelation(const std::string& text) {
    std::string s = lowerAscii(oneLine(text));
    static const char* const kArticles[] = {"my ", "the ", "a ", "an ", "who "};
    for (const char* a : kArticles) {
        const std::string prefix(a);
        if (s.rfind(prefix, 0) == 0) {
            s = trim(s.substr(prefix.size()));
            break;
        }
    }
    std::string out;
    bool space = false;
    for (unsigned char c : s) {
        if (c >= 0x80) {
            out += static_cast<char>(c);
            space = false;
        } else if (std::isalnum(c) || c == '-') {
            out += static_cast<char>(c);
            space = false;
        } else if (c == ' ') {
            if (!out.empty() && !space) out += ' ';
            space = true;
        }
    }
    s = trim(out);

    static const std::map<std::string, std::string> kSynonyms = {
        {"father", "dad"}, {"dad", "dad"}, {"mother", "mum"},
        {"mom", "mum"},    {"mum", "mum"},
    };
    if (s.find(' ') == std::string::npos) {
        const auto it = kSynonyms.find(s);
        if (it != kSynonyms.end()) return it->second;
    }
    return s;
}

std::string lineKey(const Line& line) {
    if (line.kind == "alias") return "alias\x1f" + normalizeName(line.text);
    if (line.kind == "login") return "login\x1f" + loginKey(line.text);
    if (line.kind == "role" || line.kind == "relation")
        return line.kind + "\x1f" + canonicalRelation(line.text);
    return line.kind + "\x1f" + normalizeName(line.text);
}

bool hasRole(const Object& o, const std::string& role) {
    for (const Line& line : o.lines) {
        if (line.kind != "role" && line.kind != "relation") continue;
        if (canonicalRelation(line.text) == role) return true;
    }
    return false;
}

bool hasLogin(const Object& o, const std::string& key) {
    for (const Line& line : o.lines) {
        if (line.kind == "login" && loginKey(line.text) == key) return true;
    }
    return false;
}

bool personCategory(const Object& o) {
    const std::string cat = lowerAscii(trim(o.category));
    return cat == "human" || cat == "person";
}

bool asciiAlias(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

bool wordChar(unsigned char c) {
    return std::isalnum(c) || c >= 0x80;
}

// ASCII aliases use word boundaries, so "CO" does not match "conversation".
// Non-ASCII aliases (for example a Chinese title) use plain substring search.
bool matchAlias(const std::string& hay, const std::string& pin) {
    if (pin.empty()) return false;
    if (!asciiAlias(pin)) return hay.find(pin) != std::string::npos;

    size_t p = 0;
    while ((p = hay.find(pin, p)) != std::string::npos) {
        const bool left = p == 0 || !wordChar(static_cast<unsigned char>(hay[p - 1]));
        const bool right = p + pin.size() >= hay.size() ||
                           !wordChar(static_cast<unsigned char>(hay[p + pin.size()]));
        if (left && right) return true;
        ++p;
    }
    return false;
}

bool containsAlias(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    const std::string hay = lowerAscii(haystack);
    const std::string pin = lowerAscii(needle);
    if (matchAlias(hay, pin)) return true;

    // A stored alias such as "the android book" must still match the user's
    // "that android book". Try without a leading article.
    static const char* const kArticles[] = {"the ", "a ", "an "};
    for (const char* article : kArticles) {
        const std::string prefix(article);
        if (pin.rfind(prefix, 0) == 0 &&
            matchAlias(hay, pin.substr(prefix.size())))
            return true;
    }
    return false;
}

bool mentions(const Object& o, const std::string& text) {
    if (text.empty()) return false;
    if (containsAlias(text, o.name)) return true;
    for (const Line& line : o.lines) {
        if (line.kind != "alias") continue;
        for (const std::string& alias : split(line.text, ','))
            if (containsAlias(text, trim(alias))) return true;
        for (const std::string& alias : split(line.text, ';'))
            if (containsAlias(text, trim(alias))) return true;
    }
    return false;
}

bool mentionsAny(const Object& o, const std::vector<std::string>& focus) {
    for (const std::string& word : focus)
        if (mentions(o, word)) return true;
    return false;
}

// Relevance is what admits an object to the details section. Recency is only
// a tie-breaker; on its own it must not make an unrelated object relevant.
int relevanceScore(const Object& o, const ViewContext& ctx) {
    int score = 0;
    // The AI's own object is always relevant.
    if (hasRole(o, "myself")) score += 60;
    // Focus is the app-owned anchor for this turn.
    if (mentionsAny(o, ctx.focus)) score += 120;
    if (!ctx.user_message.empty() && mentions(o, ctx.user_message))
        score += 100;
    if (mentions(o, ctx.task)) score += 40;
    if (mentions(o, ctx.recent)) score += 20;
    return score;
}

int scoreObject(const Object& o, const ViewContext& ctx, size_t pos,
                size_t total) {
    // Deterministic recency: later objects in the file get a small bonus.
    const int recency =
        static_cast<int>(10 * pos / std::max<size_t>(1, total));
    return relevanceScore(o, ctx) + recency;
}

std::string kindText(const std::string& kind) {
    return kind + ": ";
}

std::string objectLine(const Object& o, size_t maxChars) {
    std::map<std::string, std::vector<std::string>> groups;
    for (const Line& line : o.lines) groups[line.kind].push_back(line.text);

    std::ostringstream row;
    row << "[" << (o.category.empty() ? "?" : o.category) << "] " << o.name
        << " (" << o.id << ")";
    bool first = true;
    static const char* const kOrder[] = {
        "alias", "login", "role", "relation", "fact", "opinion", "note"};
    for (const char* kind : kOrder) {
        const auto it = groups.find(kind);
        if (it == groups.end()) continue;
        for (const std::string& text : it->second) {
            row << (first ? " — " : "; ") << kindText(kind) << text;
            first = false;
        }
    }
    std::string out = row.str();
    if (maxChars > 0 && out.size() > maxChars)
        out = text_utf8::head(out, maxChars) + "…";
    return out;
}

std::string indexLine(const Object& o) {
    std::ostringstream row;
    row << "[" << (o.category.empty() ? "?" : o.category) << "] " << o.name
        << " (" << o.id << ")";
    for (const Line& line : o.lines) {
        if (line.kind != "alias") continue;
        std::string aliases;
        for (const std::string& alias : split(line.text, ',')) {
            if (!aliases.empty()) aliases += ", ";
            aliases += trim(alias);
        }
        if (!aliases.empty()) {
            row << " — alias: " << aliases;
            break;
        }
    }
    return row.str();
}

std::string fnvHex(const std::string& s) {
    unsigned int h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", h);
    return buf;
}

std::string slugify(const std::string& category, const std::string& name) {
    std::string cat = lowerAscii(trim(category));
    if (cat.empty() || cat == "?") cat = "obj";
    std::string slug;
    for (unsigned char c : name) {
        if (c < 0x80 && std::isalnum(c)) {
            slug += static_cast<char>(std::tolower(c));
        } else if (!slug.empty() && slug.back() != '-') {
            slug += '-';
        }
    }
    while (!slug.empty() && slug.front() == '-') slug.erase(slug.begin());
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = fnvHex(name);
    return cat + "-" + slug;
}

std::string uniqueSlug(const Tracker& tracker, const std::string& category,
                       const std::string& name) {
    const std::string base = slugify(category, name);
    std::string id = base;
    int n = 2;
    for (;;) {
        bool used = false;
        for (const Object& o : tracker.objects) {
            if (lowerAscii(o.id) == lowerAscii(id)) {
                used = true;
                break;
            }
        }
        if (!used) return id;
        id = base + "-" + std::to_string(n++);
    }
}

std::string uncertainId(const Tracker& tracker) {
    const std::string base =
        "?" + fnvHex(std::to_string(std::time(nullptr)) + ":" +
                     std::to_string(tracker.objects.size()));
    std::string id = base.substr(0, 6); // "?xxxxx"
    int n = 2;
    for (;;) {
        bool used = false;
        for (const Object& o : tracker.objects) {
            if (lowerAscii(o.id) == lowerAscii(id)) {
                used = true;
                break;
            }
        }
        if (!used) return id;
        id = base.substr(0, 5) + std::to_string(n++);
    }
}

std::string inferKind(const std::string& category) {
    const std::string cat = lowerAscii(trim(category));
    if (cat == "human" || cat == "person" || cat == "ai") return "role";
    return "fact";
}

bool parseIdentityLine(const std::string& raw, Line& line) {
    const std::string s = trim(raw);
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    const std::string kind = lowerAscii(trim(s.substr(0, colon)));
    if (kind != "alias" && kind != "role" && kind != "relation" &&
        kind != "fact" && kind != "opinion" && kind != "note" &&
        kind != "login")
        return false;
    line.kind = kind;
    line.text = facts::unescapeField(trim(s.substr(colon + 1)));
    return !trim(line.text).empty();
}

Object parseHeader(const std::string& line) {
    Object o;
    if (line.empty() || line[0] != '[') return o;
    const size_t close = line.find(']');
    if (close == std::string::npos) return o;
    o.category = trim(line.substr(1, close - 1));
    const std::vector<std::string> parts =
        split(line.substr(close + 1), '|');
    if (!parts.empty()) o.name = trim(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string part = trim(parts[i]);
        const size_t colon = part.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = lowerAscii(trim(part.substr(0, colon)));
        const std::string value =
            facts::unescapeField(trim(part.substr(colon + 1)));
        if (key == "id") {
            o.id = value;
        } else if (key == "status") {
            o.status = value;
        } else if (key == "created") {
            o.created = value;
        } else if (key == "source") {
            o.source = value;
        } else if (key == "merge-reason") {
            o.mergeReason = value;
        } else if (key == "merged-from") {
            for (const std::string& id : split(value, ',')) {
                if (!trim(id).empty()) o.mergedFrom.push_back(trim(id));
            }
        }
    }
    return o;
}

bool matches(const Object& o, const std::string& query) {
    const std::string q = lowerAscii(trim(query));
    if (q.empty()) return false;
    if (lowerAscii(trim(o.id)) == q) return true;
    if (lowerAscii(trim(o.name)) == q) return true;
    for (const Line& line : o.lines) {
        if (line.kind != "alias") continue;
        for (const std::string& alias : split(line.text, ',')) {
            if (lowerAscii(trim(alias)) == q) return true;
        }
        for (const std::string& alias : split(line.text, ';')) {
            if (lowerAscii(trim(alias)) == q) return true;
        }
    }
    return false;
}

bool parseSpec(const std::string& spec, std::string& category,
               std::string& name, std::vector<Line>& lines,
               std::string& note) {
    std::string s = trim(spec);
    category = "?";
    if (!s.empty() && s[0] == '[') {
        const size_t close = s.find(']');
        if (close == std::string::npos) {
            note = "missing ] after the category";
            return false;
        }
        category = trim(s.substr(1, close - 1));
        s = trim(s.substr(close + 1));
    }

    const size_t colon = s.find(':');
    const std::string namePart = colon == std::string::npos
                                     ? s
                                     : s.substr(0, colon);
    name = trim(namePart);
    if (name.empty()) {
        note = "missing object name";
        return false;
    }

    const std::string items =
        colon == std::string::npos ? std::string() : s.substr(colon + 1);
    for (std::string item : split(items, ';')) {
        item = trim(item);
        if (item.empty()) continue;

        Line line;
        const size_t c = item.find(':');
        const std::string maybe =
            c == std::string::npos ? std::string()
                                   : lowerAscii(trim(item.substr(0, c)));
        if (maybe == "alias" || maybe == "role" || maybe == "relation" ||
            maybe == "fact" || maybe == "opinion" || maybe == "note" ||
            maybe == "login") {
            line.kind = maybe;
            line.text = trim(item.substr(c + 1));
        } else {
            line.kind = inferKind(category);
            line.text = item;
        }
        if (!line.text.empty()) lines.push_back(std::move(line));
    }
    return true;
}

} // namespace

std::string path() { return configSubpath("identity-tracker.mem"); }

std::string render(const Tracker& tracker) {
    std::ostringstream out;
    size_t count = 0;
    for (const Object& o : tracker.objects) {
        if (o.status != "active") continue;

        std::map<std::string, std::vector<std::string>> groups;
        for (const Line& line : o.lines) groups[line.kind].push_back(line.text);

        std::ostringstream row;
        row << "[" << (o.category.empty() ? "?" : o.category) << "] " << o.name
            << " (" << o.id << ")";
        bool first = true;
        static const char* const kOrder[] = {
            "alias", "login", "role", "relation", "fact", "opinion", "note"};
        for (const char* kind : kOrder) {
            const auto it = groups.find(kind);
            if (it == groups.end()) continue;
            for (const std::string& text : it->second) {
                row << (first ? " — " : "; ") << kind << ": " << text;
                first = false;
            }
        }

        if (count == 0)
            out << "# Identity tracker (authoritative for objects and facts)\n\n";
        out << row.str() << "\n";
        ++count;
    }
    if (count == 0) return std::string();

    std::string text = out.str();
    if (text.size() > kRenderChars)
        text = text_utf8::head(text, kRenderChars) +
               "\n…(more objects omitted)…\n";
    return text;
}

View renderView(const Tracker& tracker, const ViewContext& ctx,
                size_t detail_budget, size_t index_budget) {
    View view;
    size_t total = 0;
    for (const Object& o : tracker.objects)
        if (o.status == "active") ++total;
    if (total == 0) return view;

    // The caller owns focus, but the renderer enforces the bounds: at most
    // three anchors, each short enough to be specific.
    ViewContext use = ctx;
    use.focus.clear();
    for (const std::string& f : ctx.focus) {
        if (use.focus.size() >= 3) break;
        const std::string clean = trim(f);
        if (clean.empty() || clean.size() > 200) continue;
        use.focus.push_back(clean);
    }

    struct Ranked {
        size_t index = 0;
        int score = 0;
        bool relevant = false;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(total);
    for (size_t i = 0; i < tracker.objects.size(); ++i) {
        if (tracker.objects[i].status != "active") continue;
        const Object& o = tracker.objects[i];
        ranked.push_back({i, scoreObject(o, use, ranked.size(), total),
                          relevanceScore(o, use) > 0});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [&](const Ranked& a, const Ranked& b) {
                         if (a.score != b.score) return a.score > b.score;
                         return tracker.objects[a.index].id <
                                tracker.objects[b.index].id;
                     });

    std::string details;
    std::string index;
    std::set<size_t> chosen;

    for (const Ranked& r : ranked) {
        const Object& o = tracker.objects[r.index];
        const bool forced = hasRole(o, "myself");
        if (!forced && !r.relevant) continue;
        const std::string line = objectLine(o, 1200) + "\n";
        if (!forced && details.size() + line.size() > detail_budget) continue;
        details += line;
        chosen.insert(r.index);
        ++view.detailed;
    }

    for (const Ranked& r : ranked) {
        if (chosen.count(r.index)) continue;
        const std::string line = indexLine(tracker.objects[r.index]) + "\n";
        if (index.size() + line.size() > index_budget) {
            ++view.omitted;
            continue;
        }
        index += line;
        ++view.indexed;
    }

    if (!details.empty()) {
        view.text +=
            "# Identity tracker — details (relevant to this turn)\n\n" +
            details;
    }
    if (!index.empty() || view.omitted > 0) {
        if (!view.text.empty()) view.text += "\n";
        view.text +=
            "# Identity tracker — index (known, not expanded)\n\n" + index;
        if (view.omitted > 0)
            view.text +=
                "…(" + std::to_string(view.omitted) + " more objects)\n";
    }
    return view;
}

const Object* primaryWithRole(const Tracker& tracker,
                              const std::string& role) {
    const std::string wanted = canonicalRelation(role);
    for (const Object& o : tracker.objects) {
        if (o.status != "active") continue;
        if (hasRole(o, wanted)) return &o;
    }
    return nullptr;
}

const Object* objectForLogin(const Tracker& tracker,
                             const std::string& loginId) {
    const std::string key = loginKey(loginId);
    if (key.empty()) return nullptr;

    for (const Object& o : tracker.objects) {
        if (o.status != "active" || !hasLogin(o, key)) continue;
        return &o;
    }
    // Older trackers have no login line yet. A person object whose exact
    // name or alias is the login-id is an unambiguous match.
    for (const Object& o : tracker.objects) {
        if (o.status != "active" || !personCategory(o)) continue;
        if (matches(o, loginId)) return &o;
    }
    return nullptr;
}

bool bindLogin(Tracker& tracker, const std::string& loginId,
               std::string& note) {
    note.clear();
    const std::string clean = trim(loginId);
    const std::string key = loginKey(clean);
    if (key.empty()) {
        note = "empty login-id";
        return false;
    }

    for (const Object& o : tracker.objects) {
        if (o.status != "active" || !hasLogin(o, key)) continue;
        return true;
    }

    for (Object& o : tracker.objects) {
        if (o.status != "active" || !personCategory(o)) continue;
        if (!matches(o, clean)) continue;
        o.lines.push_back({"login", clean});
        note = "bound login-id " + clean + " to [" + o.category + "] " +
               o.name + " (" + o.id + ")";
        return true;
    }

    Object fresh;
    fresh.category = "Human";
    fresh.name = clean;
    fresh.created = today();
    fresh.source = "login";
    fresh.id = uniqueSlug(tracker, fresh.category, clean);
    fresh.lines.push_back({"login", clean});
    tracker.objects.push_back(std::move(fresh));
    const Object& made = tracker.objects.back();
    note = "created [" + made.category + "] " + made.name + " (" + made.id +
           ") for login-id " + clean;
    return true;
}

std::vector<std::string> focusFromText(const Tracker& tracker,
                                       const std::string& text,
                                       size_t max) {
    struct Hit {
        size_t length = 0;
        std::string name;
    };
    std::vector<Hit> hits;
    for (const Object& o : tracker.objects) {
        if (o.status != "active" || o.name.empty()) continue;
        size_t best = 0;
        if (containsAlias(text, o.name)) best = o.name.size();
        for (const Line& line : o.lines) {
            if (line.kind != "alias") continue;
            for (const std::string& alias : split(line.text, ',')) {
                const std::string a = trim(alias);
                if (!a.empty() && containsAlias(text, a))
                    best = std::max(best, a.size());
            }
            for (const std::string& alias : split(line.text, ';')) {
                const std::string a = trim(alias);
                if (!a.empty() && containsAlias(text, a))
                    best = std::max(best, a.size());
            }
        }
        if (best > 0) hits.push_back({best, o.name});
    }

    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.length != b.length) return a.length > b.length;
        return a.name < b.name;
    });

    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const Hit& hit : hits) {
        if (out.size() >= max) break;
        if (hit.name.size() > 200) continue;
        if (!seen.insert(lowerAscii(hit.name)).second) continue;
        out.push_back(hit.name);
    }
    return out;
}

const Object* find(const Tracker& tracker, const std::string& idOrName) {
    const Object* inactive = nullptr;
    for (const Object& o : tracker.objects) {
        if (!matches(o, idOrName)) continue;
        if (o.status == "active") return &o;
        if (!inactive) inactive = &o;
    }
    return inactive;
}

Object* find(Tracker& tracker, const std::string& idOrName) {
    for (Object& o : tracker.objects) {
        if (o.status == "active" && matches(o, idOrName)) return &o;
    }
    for (Object& o : tracker.objects) {
        if (matches(o, idOrName)) return &o;
    }
    return nullptr;
}

bool track(Tracker& tracker, const std::string& spec, std::string& id,
           std::string& note) {
    std::string category, name;
    std::vector<Line> lines;
    if (!parseSpec(spec, category, name, lines, note)) return false;
    if (category.empty()) category = "?";

    Object* object = find(tracker, name);
    if (object) {
        if (object->status != "active") {
            note = "object " + object->id + " is " + object->status;
            return false;
        }
        // An uncertain object can be promoted once a category is known.
        if ((object->category == "?" || object->category.empty()) &&
            category != "?")
            object->category = category;
    } else {
        Object fresh;
        fresh.category = category;
        fresh.name = name;
        fresh.created = today();
        fresh.source = "user";
        if (!name.empty() && name[0] == '?')
            fresh.id = name;
        else if (category == "?")
            fresh.id = uncertainId(tracker);
        else
            fresh.id = uniqueSlug(tracker, category, name);
        tracker.objects.push_back(std::move(fresh));
        object = &tracker.objects.back();
    }

    std::set<std::string> keys;
    for (const Line& line : object->lines) keys.insert(lineKey(line));
    size_t added = 0;
    for (Line& line : lines) {
        const std::string key = lineKey(line);
        if (keys.count(key)) continue;
        keys.insert(key);
        object->lines.push_back(std::move(line));
        ++added;
    }

    id = object->id;
    note = (added == 0 ? "already current: " : "tracked: ") + std::string("[") +
           object->category + "] " + object->name + " (" + object->id + ")";
    return true;
}

bool addLine(Tracker& tracker, const std::string& idOrName,
             const std::string& kind, const std::string& text,
             std::string& note) {
    const std::string k = lowerAscii(trim(kind));
    if (k == "login") {
        note = "login bindings are managed by /login, not by <track>";
        return false;
    }
    if (k != "alias" && k != "role" && k != "relation" && k != "fact" &&
        k != "opinion" && k != "note") {
        note = "unknown identity kind: " + kind;
        return false;
    }

    Object* object = find(tracker, idOrName);
    if (!object) {
        note = "object not found: " + idOrName;
        return false;
    }
    if (object->status != "active") {
        note = "object " + object->id + " is " + object->status;
        return false;
    }

    std::string clean = oneLine(text);
    if (clean.empty()) {
        note = "empty identity text";
        return false;
    }
    if (clean.size() > 200)
        clean = text_utf8::head(clean, 200) + "…";

    Line line{k, clean};
    const std::string key = lineKey(line);
    for (const Line& existing : object->lines) {
        if (lineKey(existing) == key) {
            note = "already present: " + clean;
            return false;
        }
    }
    object->lines.push_back(std::move(line));
    note = "recorded " + k + " on [" + object->category + "] " + object->name +
           " (" + object->id + "): " + clean;
    return true;
}

bool merge(Tracker& tracker, const std::string& from, const std::string& into,
           const std::string& reason, std::string& note) {
    Object* source = find(tracker, from);
    Object* target = find(tracker, into);
    if (!source || !target) {
        note = "object not found: " + std::string(!source ? from : into);
        return false;
    }
    if (source == target) {
        note = "cannot merge an object into itself";
        return false;
    }
    if (source->status != "active") {
        note = "source object " + source->id + " is " + source->status;
        return false;
    }
    if (target->status != "active") {
        note = "target object " + target->id + " is " + target->status;
        return false;
    }

    std::set<std::string> keys;
    for (const Line& line : target->lines) keys.insert(lineKey(line));

    size_t carried = 0;
    // The source's canonical name becomes an alias on the target, so the
    // merged object answers to both names. An uncertain ?id is deliberately
    // not carried; that is the placeholder the user wants to abandon.
    if (!source->name.empty() && source->name[0] != '?' &&
        lowerAscii(source->name) != lowerAscii(target->name)) {
        Line alias{"alias", source->name};
        const std::string key = lineKey(alias);
        if (keys.insert(key).second) {
            target->lines.push_back(std::move(alias));
            ++carried;
        }
    }

    for (const Line& line : source->lines) {
        const std::string key = lineKey(line);
        if (keys.count(key)) continue;
        keys.insert(key);
        target->lines.push_back(line);
        ++carried;
    }

    target->mergedFrom.push_back(source->id);
    for (const std::string& id : source->mergedFrom)
        target->mergedFrom.push_back(id);
    std::sort(target->mergedFrom.begin(), target->mergedFrom.end());
    target->mergedFrom.erase(
        std::unique(target->mergedFrom.begin(), target->mergedFrom.end()),
        target->mergedFrom.end());
    if (!trim(reason).empty()) target->mergeReason = oneLine(reason);

    source->status = "merged-into:" + target->id;
    note = "merged [" + source->category + "] " + source->name + " (" +
           source->id + ") into [" + target->category + "] " + target->name +
           " (" + target->id + "); " + std::to_string(carried) +
           " identities carried";
    return true;
}

bool forget(Tracker& tracker, const std::string& idOrName, std::string& note) {
    Object* object = find(tracker, idOrName);
    if (!object) {
        note = "object not found: " + idOrName;
        return false;
    }
    if (object->status == "retired") {
        note = "object " + object->id + " is already retired";
        return false;
    }
    if (object->status != "active") {
        note = "object " + object->id + " is " + object->status;
        return false;
    }
    object->status = "retired";
    note = "retired [" + object->category + "] " + object->name + " (" +
           object->id + ")";
    return true;
}

static Tracker parseFile(const std::string& file);

static bool saveTo(const Tracker& tracker, const std::string& file) {
    if (file.empty()) return false;
    ensureConfigDir();

    const std::string tmp = file + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "# ai-chat identity tracker v1\n\n";
    for (const Object& o : tracker.objects) {
        out << "[" << sanitizeHeader(o.category.empty() ? "?" : o.category)
            << "] " << sanitizeHeader(o.name) << " | id: "
            << sanitizeHeader(o.id) << " | status: "
            << sanitizeHeader(o.status) << " | created: "
            << sanitizeHeader(o.created) << " | source: "
            << sanitizeHeader(o.source);
        if (!o.mergedFrom.empty()) {
            out << " | merged-from: ";
            for (size_t i = 0; i < o.mergedFrom.size(); ++i) {
                if (i) out << ",";
                out << sanitizeHeader(o.mergedFrom[i]);
            }
        }
        if (!o.mergeReason.empty())
            out << " | merge-reason: " << sanitizeHeader(o.mergeReason);
        out << "\n";
        for (const Line& line : o.lines) {
            out << "  " << line.kind << ": "
                << facts::escapeField(oneLine(line.text)) << "\n";
        }
        out << "\n";
    }
    out.close();
    if (!out) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), file.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool save(const Tracker& tracker) { return saveTo(tracker, path()); }

std::string historyPath() {
    return configSubpath("identity-history.mem");
}

bool compact(Tracker& tracker, size_t keepRecent, std::string& note) {
    std::vector<size_t> tombstones;
    for (size_t i = 0; i < tracker.objects.size(); ++i) {
        if (tracker.objects[i].status != "active") tombstones.push_back(i);
    }
    if (tombstones.size() <= keepRecent) {
        note = "no tombstones to archive";
        return true;
    }

    // Keep the newest objects; archive the rest. ISO dates sort correctly.
    std::sort(tombstones.begin(), tombstones.end(), [&](size_t a, size_t b) {
        const Object& x = tracker.objects[a];
        const Object& y = tracker.objects[b];
        if (x.created != y.created) return x.created > y.created;
        return x.id < y.id;
    });

    std::vector<Object> moving;
    for (size_t i = keepRecent; i < tombstones.size(); ++i)
        moving.push_back(tracker.objects[tombstones[i]]);

    Tracker history = parseFile(historyPath());
    std::set<std::string> ids;
    for (const Object& o : history.objects) ids.insert(lowerAscii(o.id));
    size_t added = 0;
    for (const Object& o : moving) {
        if (!ids.insert(lowerAscii(o.id)).second) continue;
        history.objects.push_back(o);
        ++added;
    }
    if (!saveTo(history, historyPath())) {
        note = "could not write identity-history.mem";
        return false;
    }

    std::set<std::string> moved;
    for (const Object& o : moving) moved.insert(lowerAscii(o.id));
    tracker.objects.erase(
        std::remove_if(tracker.objects.begin(), tracker.objects.end(),
                       [&](const Object& o) {
                           return o.status != "active" &&
                                  moved.count(lowerAscii(o.id)) > 0;
                       }),
        tracker.objects.end());

    note = "archived " + std::to_string(added) +
           " tombstones to identity-history.mem";
    return true;
}

void migrateLegacyFacts() {
    const std::string trackerPath = path();
    if (trackerPath.empty() || ::access(trackerPath.c_str(), F_OK) == 0) return;

    const std::string taskPath = configSubpath("autonomy-facts.mem");
    if (taskPath.empty()) return;
    std::ifstream in(taskPath);
    if (!in) return;

    std::vector<std::string> keep;
    std::vector<std::string> migrated;
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> parts = split(line, '\t');
        const bool identity =
            parts.size() >= 10 &&
            (line.rfind("fact\t", 0) == 0 || line.rfind("old\t", 0) == 0) &&
            trim(parts[9]) == "1";
        if (identity) {
            const std::string value = facts::unescapeField(parts[3]);
            if (!trim(value).empty()) migrated.push_back(value);
        } else {
            keep.push_back(line);
        }
    }
    in.close();
    if (migrated.empty()) return;

    Tracker tracker;
    Object object;
    object.id = "?legacy";
    object.category = "?";
    object.name = "?legacy";
    object.created = today();
    object.source = "migration";
    object.lines.push_back({"note", "migrated from the old identity facts"});
    for (const std::string& value : migrated)
        object.lines.push_back({"fact", value});
    tracker.objects.push_back(std::move(object));
    if (!save(tracker)) return;

    const std::string tmp = taskPath + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return;
    for (const std::string& kept : keep) out << kept << "\n";
    out.close();
    if (out)
        std::rename(tmp.c_str(), taskPath.c_str());
    else
        std::remove(tmp.c_str());
}

static Tracker parseFile(const std::string& file) {
    Tracker tracker;
    std::ifstream in(file);
    if (!in) return tracker;

    std::string line;
    size_t current = static_cast<size_t>(-1);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            Object object = parseHeader(line);
            if (object.name.empty() && object.id.empty()) continue;
            tracker.objects.push_back(std::move(object));
            current = tracker.objects.size() - 1;
        } else if (current != static_cast<size_t>(-1)) {
            Line parsed;
            if (parseIdentityLine(line, parsed))
                tracker.objects[current].lines.push_back(std::move(parsed));
        }
    }

    std::set<std::string> ids;
    for (Object& o : tracker.objects) {
        if (o.name.empty()) o.name = o.id.empty() ? "unnamed" : o.id;
        if (o.id.empty()) o.id = slugify(o.category, o.name);
        const std::string base = o.id;
        int n = 2;
        while (ids.count(lowerAscii(o.id))) o.id = base + "-" + std::to_string(n++);
        ids.insert(lowerAscii(o.id));
        if (o.created.empty()) o.created = today();
        if (o.status.empty()) o.status = "active";
        if (o.source.empty()) o.source = "user";
        if (o.category.empty()) o.category = "?";
    }
    return tracker;
}

Tracker load() {
    migrateLegacyFacts();
    return parseFile(path());
}

} // namespace identity
