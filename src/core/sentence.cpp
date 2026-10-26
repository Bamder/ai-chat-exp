#include "aichat/core/sentence.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "aichat/core/chat.h" // WAIT_TAG
#include "aichat/core/cognition.h" // SCA_TAG

namespace {

// Longest suffix of s that is also a (proper) prefix of pat.
size_t holdbackSuffix(const std::string& s, const char* pat) {
    size_t patLen = strlen(pat);
    size_t maxK = std::min(s.size(), patLen - 1);
    for (size_t k = maxK; k > 0; --k)
        if (s.compare(s.size() - k, k, pat, 0, k) == 0) return k;
    return 0;
}

// Parses "<timer N/>" at `pos` (spaces and '=' are tolerated). On success
// `end` is the position just past the tag. `secs` is clamped to the normal
// 30-180 range unless the user explicitly gave an exact time, in which case
// it is only kept within a sane hard limit. A number well above the range
// saturates instead of overflowing.
bool parseTimerTag(const std::string& s, size_t pos, size_t& end, int& secs,
                   bool allowLongTimer) {
    static const char* open = "<timer";
    const size_t openLen = strlen(open);
    if (pos + openLen > s.size() || s.compare(pos, openLen, open) != 0)
        return false;

    size_t i = pos + openLen;
    auto skipSpace = [&] {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    };
    skipSpace();
    if (i < s.size() && s[i] == '=') {
        ++i;
        skipSpace();
    }

    const size_t digitsAt = i;
    long v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (v < 100000) v = v * 10 + (s[i] - '0');
        ++i;
    }
    if (i == digitsAt) return false;
    skipSpace();
    if (i + 1 >= s.size() || s[i] != '/' || s[i + 1] != '>') return false;

    end = i + 2;
    const long lo = allowLongTimer ? 1 : TIMER_MIN_SEC;
    const long hi = allowLongTimer ? TIMER_HARD_MAX_SEC : TIMER_MAX_SEC;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    secs = static_cast<int>(v);
    return true;
}

// Parses the attributes of <track id="..." kind="..." text="..."/>. Values
// may not contain a double quote or the sequence "/>" (keep aliases short).
bool parseTrackAttrs(const std::string& body, TrackRequest& req) {
    size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() &&
               (std::isspace(static_cast<unsigned char>(body[i])) ||
                body[i] == '/'))
            ++i;
        if (i >= body.size()) break;

        const size_t keyAt = i;
        while (i < body.size() &&
               (std::isalnum(static_cast<unsigned char>(body[i])) ||
                body[i] == '-' || body[i] == '_'))
            ++i;
        if (i == keyAt) return false;
        const std::string key = body.substr(keyAt, i - keyAt);

        while (i < body.size() &&
               std::isspace(static_cast<unsigned char>(body[i])))
            ++i;
        if (i >= body.size() || body[i] != '=') return false;
        ++i;
        while (i < body.size() &&
               std::isspace(static_cast<unsigned char>(body[i])))
            ++i;
        if (i >= body.size() || body[i] != '"') return false;
        ++i;
        const size_t valueAt = i;
        while (i < body.size() && body[i] != '"') ++i;
        if (i >= body.size()) return false;
        const std::string value = body.substr(valueAt, i - valueAt);
        ++i;

        if (key == "id") req.id = value;
        else if (key == "kind") req.kind = value;
        else if (key == "text") req.text = value;
    }
    if (req.kind.empty()) req.kind = "alias";
    return !req.id.empty() && !req.text.empty();
}

std::string storyObjectAttr(const std::string& attrs) {
    size_t p = attrs.find("object");
    while (p != std::string::npos) {
        const bool left = p == 0 ||
                          std::isspace(static_cast<unsigned char>(attrs[p - 1]));
        if (left) break;
        p = attrs.find("object", p + 1);
    }
    if (p == std::string::npos) return "";
    p += 6;
    while (p < attrs.size() &&
           std::isspace(static_cast<unsigned char>(attrs[p])))
        ++p;
    if (p >= attrs.size() || attrs[p] != '=') return "";
    ++p;
    while (p < attrs.size() &&
           std::isspace(static_cast<unsigned char>(attrs[p])))
        ++p;
    if (p >= attrs.size() || attrs[p] != '"') return "";
    ++p;
    const size_t start = p;
    while (p < attrs.size() && attrs[p] != '"') ++p;
    if (p >= attrs.size()) return "";
    return attrs.substr(start, p - start);
}

bool parseStoryTag(const std::string& s, size_t pos, size_t& end,
                   StoryRequest& req) {
    static const char* open = "<story";
    const size_t openLen = strlen(open);
    if (pos + openLen > s.size() || s.compare(pos, openLen, open) != 0)
        return false;
    if (pos + openLen < s.size() &&
        !std::isspace(static_cast<unsigned char>(s[pos + openLen])) &&
        s[pos + openLen] != '>')
        return false;
    const size_t gt = s.find('>', pos + openLen);
    if (gt == std::string::npos) return false;
    const size_t close = s.find("</story>", gt + 1);
    if (close == std::string::npos) return false;
    req.object =
        storyObjectAttr(s.substr(pos + openLen, gt - (pos + openLen)));
    req.body = s.substr(gt + 1, close - (gt + 1));
    end = close + 8;
    return true;
}

bool parseTrackTag(const std::string& s, size_t pos, size_t& end,
                   TrackRequest& req) {
    static const char* open = "<track";
    const size_t openLen = strlen(open);
    if (pos + openLen > s.size()) return false;
    if (s.compare(pos, openLen, open) != 0) return false;
    if (pos + openLen < s.size() &&
        !std::isspace(static_cast<unsigned char>(s[pos + openLen])))
        return false;

    const size_t close = s.find("/>", pos + openLen);
    if (close == std::string::npos) return false;
    if (!parseTrackAttrs(s.substr(pos + openLen, close - (pos + openLen)),
                         req))
        return false;
    end = close + 2;
    return true;
}

// True when the tail starting at `pos` could still grow into a complete
// timer tag, so it must not be printed yet.
bool couldBeTimerTagPrefix(const std::string& s, size_t pos) {
    static const char* open = "<timer";
    const size_t openLen = strlen(open);
    const size_t n = s.size() - pos;
    const size_t k = std::min(n, openLen);
    if (s.compare(pos, k, open, k) != 0) return false;
    if (n < openLen) return true;

    size_t i = pos + openLen;
    auto skipSpace = [&] {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    };
    skipSpace();
    if (i < s.size() && s[i] == '=') {
        ++i;
        skipSpace();
    }
    if (i == s.size()) return true;
    if (s[i] < '0' || s[i] > '9') return false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    skipSpace();
    if (i == s.size()) return true;
    return s[i] == '/' && i + 1 == s.size();
}

// Length of the trailing fragment that might still become a timer tag.
size_t timerTagHoldback(const std::string& s) {
    for (size_t pos = s.rfind('<'); pos != std::string::npos;
         pos = pos == 0 ? std::string::npos : s.rfind('<', pos - 1)) {
        if (couldBeTimerTagPrefix(s, pos)) return s.size() - pos;
    }
    return 0;
}

// Hold a <track ...> fragment until its closing "/>" arrives. A complete but
// malformed tag is returned to the text stream instead of disappearing.
size_t trackTagHoldback(const std::string& s) {
    const size_t pos = s.rfind("<track");
    if (pos == std::string::npos) return 0;
    if (s.find("/>", pos) != std::string::npos) return 0;
    return s.size() - pos;
}

// Hold a <lesson> block until its closing tag arrives. The body is opaque,
// so a tag inside it is not parsed as another control tag.
size_t lessonTagHoldback(const std::string& s) {
    const size_t pos = s.rfind("<lesson>");
    if (pos == std::string::npos) return 0;
    if (s.find("</lesson>", pos + 8) != std::string::npos) return 0;
    return s.size() - pos;
}

// Hold a <story ...> block until its closing tag arrives.
size_t storyTagHoldback(const std::string& s) {
    const size_t pos = s.rfind("<story");
    if (pos == std::string::npos) return 0;
    if (s.find("</story>", pos) != std::string::npos) return 0;
    return s.size() - pos;
}

struct TagHit {
    size_t pos = std::string::npos;
    size_t end = 0;
    bool wait = false;
    bool cognition = false;
    bool autonomy = false;
    bool track = false;
    bool lesson = false;
    bool story = false;
    int secs = 0;
    TrackRequest request;
    std::string lessonBody;
    StoryRequest storyRequest;
};

// Earliest complete <wait/>, <sca/>, <auto/>, <timer N/> or <track .../>.
TagHit findTag(const std::string& s, bool allowLongTimer) {
    TagHit hit;

    auto consider = [&](size_t pos, size_t end, bool wait, bool cognition,
                        bool autonomy, int secs) {
        if (pos == std::string::npos) return;
        if (hit.pos == std::string::npos || pos < hit.pos) {
            hit = TagHit{};
            hit.pos = pos;
            hit.end = end;
            hit.wait = wait;
            hit.cognition = cognition;
            hit.autonomy = autonomy;
            hit.secs = secs;
        }
    };

    auto considerTrack = [&](size_t pos, size_t end, const TrackRequest& req) {
        if (hit.pos == std::string::npos || pos < hit.pos) {
            hit = TagHit{};
            hit.pos = pos;
            hit.end = end;
            hit.track = true;
            hit.request = req;
        }
    };

    auto considerLesson = [&](size_t pos, size_t end,
                              const std::string& body) {
        if (hit.pos == std::string::npos || pos < hit.pos) {
            hit = TagHit{};
            hit.pos = pos;
            hit.end = end;
            hit.lesson = true;
            hit.lessonBody = body;
        }
    };

    auto considerStory = [&](size_t pos, size_t end, const StoryRequest& req) {
        if (hit.pos == std::string::npos || pos < hit.pos) {
            hit = TagHit{};
            hit.pos = pos;
            hit.end = end;
            hit.story = true;
            hit.storyRequest = req;
        }
    };

    size_t w = s.find(WAIT_TAG);
    if (w != std::string::npos)
        consider(w, w + strlen(WAIT_TAG), true, false, false, 0);

    size_t c = s.find(SCA_TAG);
    if (c != std::string::npos)
        consider(c, c + strlen(SCA_TAG), false, true, false, 0);

    size_t a = s.find(AUTO_TAG);
    if (a != std::string::npos)
        consider(a, a + strlen(AUTO_TAG), false, false, true, 0);

    for (size_t pos = s.find("<timer"); pos != std::string::npos;
         pos = s.find("<timer", pos + 1)) {
        size_t end = 0;
        int secs = 0;
        if (!parseTimerTag(s, pos, end, secs, allowLongTimer)) continue;
        consider(pos, end, false, false, false, secs);
        break;
    }

    for (size_t pos = s.find("<track"); pos != std::string::npos;
         pos = s.find("<track", pos + 1)) {
        size_t end = 0;
        TrackRequest req;
        if (!parseTrackTag(s, pos, end, req)) continue;
        considerTrack(pos, end, req);
        break;
    }

    const size_t open = s.find("<lesson>");
    if (open != std::string::npos) {
        const size_t close = s.find("</lesson>", open + 8);
        if (close != std::string::npos) {
            const std::string body = s.substr(open + 8, close - (open + 8));
            considerLesson(open, close + 9, body);
        }
    }

    for (size_t pos = s.find("<story"); pos != std::string::npos;
         pos = s.find("<story", pos + 1)) {
        size_t end = 0;
        StoryRequest req;
        if (!parseStoryTag(s, pos, end, req)) continue;
        considerStory(pos, end, req);
        break;
    }
    return hit;
}

} // namespace

SentencePrinter::SentencePrinter(FILE* out, bool allowLongTimer)
    : out_(out), allowLongTimer_(allowLongTimer) {}

void SentencePrinter::feed(const std::string& chunk) {
    pending_ += chunk;
    drain(false);
}

void SentencePrinter::finish() { drain(true); }

void SentencePrinter::emit(const std::string& s) {
    if (s.empty()) return;
    text_ += s;
    if (out_) { fwrite(s.data(), 1, s.size(), out_); fflush(out_); }
}

void SentencePrinter::drain(bool final) {
    for (;;) {
        TagHit tag = findTag(pending_, allowLongTimer_);
        if (tag.pos != std::string::npos) {
            emit(pending_.substr(0, tag.pos));
            if (tag.wait) wait_ = true;
            else if (tag.cognition) cognition_ = true;
            else if (tag.autonomy) autonomy_ = true;
            else if (tag.track) track_.push_back(tag.request);
            else if (tag.lesson) lessons_.push_back(tag.lessonBody);
            else if (tag.story) stories_.push_back(tag.storyRequest);
            else timerSec_ = tag.secs;
            pending_.erase(0, tag.end);
            continue; // the rest may hold more tags
        }
        if (final) {
            emit(pending_);
            pending_.clear();
            return;
        }
        // Print everything except a tail that could still grow into one of
        // the tags (<wait/>, <sca/>, <auto/>, <timer N/>, <track .../>, or
        // <lesson>...</lesson>). This keeps the reply flowing as it arrives
        // instead of waiting for a sentence boundary.
        size_t hold = std::max(holdbackSuffix(pending_, WAIT_TAG),
                               holdbackSuffix(pending_, SCA_TAG));
        hold = std::max(hold, holdbackSuffix(pending_, AUTO_TAG));
        hold = std::max(hold, holdbackSuffix(pending_, "<track"));
        hold = std::max(hold, holdbackSuffix(pending_, "<lesson>"));
        hold = std::max(hold, holdbackSuffix(pending_, "<story"));
        hold = std::max(hold, timerTagHoldback(pending_));
        hold = std::max(hold, trackTagHoldback(pending_));
        hold = std::max(hold, lessonTagHoldback(pending_));
        hold = std::max(hold, storyTagHoldback(pending_));
        if (hold >= pending_.size()) return;
        size_t keep = pending_.size() - hold;
        emit(pending_.substr(0, keep));
        pending_.erase(0, keep);
        return;
    }
}
