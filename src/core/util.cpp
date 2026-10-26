#include "aichat/core/util.h"

#include <cctype>
#include <cstring>

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool startsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string lowercase(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// True when the word `w` starts at `pos` and is not followed by more letters
// (so "min" matches "5 min." but not "minimum").
bool wordAt(const std::string& s, size_t pos, const char* w) {
    size_t n = std::strlen(w);
    if (pos > s.size() || s.compare(pos, n, w) != 0) return false;
    return pos + n >= s.size() || !isAlpha(s[pos + n]);
}

bool timeUnitAt(const std::string& s, size_t pos) {
    static const char* const units[] = {
        "seconds", "second", "secs", "sec",
        "minutes", "minute", "mins", "min",
        "hours",   "hour",   "hrs",  "hr",
    };
    for (const char* u : units)
        if (wordAt(s, pos, u)) return true;
    return false;
}

bool numberWordAt(const std::string& s, size_t pos) {
    static const char* const words[] = {
        "one",    "two",     "three", "four",  "five",  "six",
        "seven",  "eight",   "nine",  "ten",   "eleven", "twelve",
        "thirteen", "fourteen", "fifteen", "twenty", "thirty",
        "forty",  "fifty",   "sixty", "ninety", "a",     "an",
        "half",   "couple",
    };
    for (const char* w : words)
        if (wordAt(s, pos, w)) return true;
    return false;
}

// Skips the small words that can sit between a number and its unit, as in
// "half an hour", "a couple of minutes" or "two hours".
size_t skipNumberFiller(const std::string& s, size_t i) {
    for (;;) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (wordAt(s, i, "a")) { i += 1; continue; }
        if (wordAt(s, i, "an")) { i += 2; continue; }
        if (wordAt(s, i, "of")) { i += 2; continue; }
        if (wordAt(s, i, "and")) { i += 3; continue; }
        if (wordAt(s, i, "couple")) { i += 6; continue; }
        return i;
    }
}

} // namespace

bool userRequestsUntimedWait(const std::string& line) {
    std::string s = lowercase(line);

    // Keep this list generous: it only grants the AI permission to use the
    // no-timer mode, which it must still choose to do itself.
    static const char* const phrases[] = {
        "wait for me",        "wait for my",      "wait until i",
        "wait till i",        "please wait",      "can you wait",
        "could you wait",     "would you wait",   "wait a moment",
        "wait a minute",      "wait a sec",       "wait a bit",
        "wait a while",       "hold on",          "one moment",
        "no timer",           "without a timer",  "without timer",
        "no time limit",      "no time-limit",    "unlimited time",
        "no countdown",       "stop the timer",   "pause the timer",
        "turn off the timer", "disable the timer", "don't time me",
        "dont time me",       "give me a moment", "give me a minute",
        "give me a sec",      "give me a second", "give me a bit",
        "give me a while",    "give me some time", "give me time",
        "give me more time",  "give me plenty of time",
        "take my time",       "let me think",     "i need a moment",
        "i need a minute",    "i need a sec",     "i need a second",
        "i need some time",   "i need more time", "i need a while",
        "i need a long time", "i need time",      "i'll need some time",
        "ill need some time", "i'll need more time", "ill need more time",
        "i'll need a while",  "ill need a while", "i'll need a long time",
        "ill need a long time", "need a long time", "need more time",
        "need some time",     "need a while",     "this may take",
        "this will take",     "may take a while", "might take a while",
        "could take a while", "will take a while", "takes a while",
        "take a while",       "a long time",      "long time",
        "be back later",      "back later",       "be right back",
        "brb",
    };
    for (const char* p : phrases)
        if (s.find(p) != std::string::npos) return true;
    return false;
}

bool userRequestsContinue(const std::string& line) {
    std::string s = lowercase(trim(line));
    static const char* const negative[] = {
        "don't", "dont", "do not", "stop", "cancel", "never mind",
        "nevermind", "hold off", "not now", "wait",
    };
    for (const char* p : negative)
        if (s.find(p) != std::string::npos) return false;

    static const char* const phrases[] = {
        "continue",    "go on",       "keep going",  "carry on",
        "resume",      "go ahead",    "proceed",     "keep working",
        "keep at it",  "back to work", "keep doing",  "you can continue",
    };
    for (const char* p : phrases)
        if (s.find(p) != std::string::npos) return true;
    return false;
}

bool userRequestsExactTime(const std::string& line) {
    std::string s = lowercase(line);

    // Clock times: "15:30", "3:30pm", "9 pm", "9 o'clock", noon, midnight.
    for (size_t i = 0; i < s.size(); ++i) {
        if (!isDigit(s[i])) continue;
        size_t j = i;
        while (j < s.size() && isDigit(s[j])) ++j;

        if (j < s.size() && s[j] == ':') {
            size_t k = j + 1;
            while (k < s.size() && isDigit(s[k])) ++k;
            if (k > j + 1) return true;
        }

        size_t k = j;
        while (k < s.size() && s[k] == ' ') ++k;
        if (wordAt(s, k, "am") || wordAt(s, k, "pm")) return true;
        if (wordAt(s, k, "o'clock") || wordAt(s, k, "oclock")) return true;
    }
    if (s.find("noon") != std::string::npos ||
        s.find("midnight") != std::string::npos)
        return true;

    // Durations with digits: "5 minutes", "1.5 hours", "90 sec".
    for (size_t i = 0; i < s.size(); ++i) {
        if (!isDigit(s[i])) continue;
        size_t j = i;
        while (j < s.size() && isDigit(s[j])) ++j;
        if (j + 1 < s.size() && s[j] == '.' && isDigit(s[j + 1])) {
            j += 2;
            while (j < s.size() && isDigit(s[j])) ++j;
        }
        j = skipNumberFiller(s, j);
        if (timeUnitAt(s, j)) return true;
    }

    // Durations written as words: "an hour", "five minutes", "half an hour".
    for (size_t i = 0; i < s.size();) {
        if (!isAlpha(s[i])) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < s.size() && isAlpha(s[i])) ++i;
        if (numberWordAt(s, start)) {
            size_t j = skipNumberFiller(s, i);
            if (timeUnitAt(s, j)) return true;
        }
    }

    // Bare numbers after a clear waiting cue: "wait till 5", "back in 10".
    static const char* const cues[] = {
        "until", "till", "til", "back in", "wait for",
    };
    for (const char* cue : cues) {
        size_t n = std::strlen(cue);
        for (size_t pos = s.find(cue); pos != std::string::npos;
             pos = s.find(cue, pos + 1)) {
            if (!wordAt(s, pos, cue)) continue;
            size_t j = pos + n;
            while (j < s.size() && s[j] == ' ') ++j;
            if (j < s.size() && isDigit(s[j])) return true;
        }
    }
    return false;
}
