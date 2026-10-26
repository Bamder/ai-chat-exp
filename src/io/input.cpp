// The timed prompt.
//
// While a countdown is running we cannot keep stdin in canonical mode: the
// terminal echoes what you type, and every timer tick would have to erase the
// line to redraw the countdown, wiping your half-written message with it.
// So on a terminal we switch to raw mode and draw the line ourselves —
// countdown, prompt and typed text together. Bracketed paste keeps a pasted
// block as one multi-line message. Pipes keep using getline.

#include "aichat/io/input.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <string>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "aichat/ui/ui.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace {

termios g_saved;
bool g_raw = false;

// Bytes already read from stdin by readLine() but not used yet. Raw-mode
// readers drain this first so a fast typist's queued Enter is not lost.
std::string g_queued;

// Put the terminal back if we are killed while it is in raw mode.
void restoreAndReraise(int sig) {
    if (g_raw) {
        static const char kPasteOff[] = "\33[?2004l";
        const ssize_t ignored =
            ::write(STDOUT_FILENO, kPasteOff, sizeof kPasteOff - 1);
        (void)ignored;
        ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
        g_raw = false;
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

bool enterRaw() {
    if (::tcgetattr(STDIN_FILENO, &g_saved) != 0) return false;
    termios raw = g_saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    // Keep CR intact: a pasted CRLF block must not arrive as two newlines.
    // readTurn already treats a lone CR (the Enter key) as submit.
    raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    g_raw = true;
    // Ask the terminal to wrap pasted text in ESC[200~ ... ESC[201~. This
    // lets readTurn keep the newlines inside a paste instead of treating the
    // first one as Enter. Terminals without support simply ignore it.
    std::printf("\33[?2004h");
    std::fflush(stdout);
    std::setlocale(LC_CTYPE, ""); // so wcwidth() knows about UTF-8
    const int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (int sig : sigs) ::signal(sig, restoreAndReraise);
    return true;
}

void leaveRaw() {
    if (!g_raw) return;
    std::printf("\33[?2004l");
    std::fflush(stdout);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    g_raw = false;
}

long long nowMs() {
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Waits up to waitMs (or forever if negative) for one byte.
bool readByte(int waitMs, char& c) {
    if (!g_queued.empty()) {
        c = g_queued.front();
        g_queued.erase(0, 1);
        return true;
    }
    for (;;) {
        pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = ::poll(&pfd, 1, waitMs);
        if (rc > 0) {
            ssize_t n = ::read(STDIN_FILENO, &c, 1);
            if (n == 1) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        if (rc < 0 && errno == EINTR) continue;
        return false;
    }
}

// Removes one character, keeping UTF-8 sequences intact.
bool eraseLastChar(std::string& s) {
    if (s.empty()) return false;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
    return true;
}

void eraseLastWord(std::string& s) {
    while (!s.empty() && s.back() == ' ') eraseLastChar(s);
    while (!s.empty() && s.back() != ' ') eraseLastChar(s);
}

// Moves a byte offset to the previous/next UTF-8 codepoint boundary, so the
// caret never lands inside a multi-byte sequence.
size_t prevCodePoint(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

size_t nextCodePoint(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

bool eraseBeforeCaret(std::string& s, size_t& caret) {
    if (caret == 0) return false;
    const size_t begin = prevCodePoint(s, caret);
    s.erase(begin, caret - begin);
    caret = begin;
    return true;
}

bool eraseAtCaret(std::string& s, size_t caret) {
    if (caret >= s.size()) return false;
    const size_t end = nextCodePoint(s, caret);
    s.erase(caret, end - caret);
    return true;
}

bool eraseWordBeforeCaret(std::string& s, size_t& caret) {
    if (caret == 0) return false;
    size_t begin = caret;
    while (begin > 0 && s[begin - 1] == ' ') begin = prevCodePoint(s, begin);
    while (begin > 0 && s[begin - 1] != ' ') begin = prevCodePoint(s, begin);
    s.erase(begin, caret - begin);
    caret = begin;
    return true;
}

// Editing keys parsed out of a terminal escape sequence. Anything not in
// this list is swallowed so its bytes never reach the message.
enum class Key {
    None,
    Left,
    Right,
    Home,
    End,
    Delete,
    PasteStart,
    PasteEnd
};

Key readEscape() {
    char c = 0;
    if (!readByte(20, c)) return Key::None;
    if (c != '[' && c != 'O') return Key::None;
    const bool csi = c == '[';

    std::string seq;
    while (readByte(20, c)) {
        seq += c;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x40 && u <= 0x7e) break; // final byte
    }
    if (seq.empty()) return Key::None;

    const char final = seq.back();
    const std::string params = seq.substr(0, seq.size() - 1);
    if (final == 'D') return Key::Left;
    if (final == 'C') return Key::Right;
    if (final == 'H') return Key::Home;
    if (final == 'F') return Key::End;
    if (csi && final == '~') {
        if (params == "1" || params == "7") return Key::Home;
        if (params == "4" || params == "8") return Key::End;
        if (params == "3") return Key::Delete;
        if (params == "200") return Key::PasteStart;
        if (params == "201") return Key::PasteEnd;
    }
    return Key::None;
}

// Collects the payload of a bracketed paste. The opening ESC[200~ was
// already consumed by readEscape(). Returns false if the closing marker
// never arrives.
bool readPaste(std::string& out) {
    static const char kEnd[] = "\33[201~";
    constexpr size_t kEndLen = sizeof kEnd - 1;
    out.clear();
    for (;;) {
        char c = 0;
        if (!readByte(5000, c)) return false;
        out += c;
        if (out.size() >= kEndLen &&
            out.compare(out.size() - kEndLen, kEndLen, kEnd) == 0) {
            out.resize(out.size() - kEndLen);
            return true;
        }
    }
}

// Normalizes pasted text for the line editor: CRLF/CR become LF, tabs
// become spaces (the column math assumes no tab stops), C0 controls and
// escape bytes are dropped, and trailing newlines are removed so a copied
// block does not leave an empty last line.
std::string cleanPaste(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
            out += '\n';
        } else if (c == '\n') {
            out += '\n';
        } else if (c == '\t') {
            out += "    ";
        } else if (c >= 0x20 && c != 0x7f) {
            out += static_cast<char>(c);
        }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return text_utf8::sanitize(out);
}

int termCols() {
    winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

int termRows() {
    winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
}

size_t decodeUtf8(const std::string& s, size_t i, wchar_t& cp) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    size_t extra;
    wchar_t v;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        v = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        v = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        v = c & 0x07;
    } else {
        cp = 0xFFFD;
        return 1;
    }
    if (i + extra >= s.size()) {
        cp = 0xFFFD;
        return 1;
    }
    for (size_t k = 1; k <= extra; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) {
            cp = 0xFFFD;
            return 1;
        }
        v = static_cast<wchar_t>((v << 6) | (cc & 0x3F));
    }
    cp = v;
    return extra + 1;
}

// wcwidth() reports -1 for characters its locale does not know. A UTF-8
// terminal still draws them, so the prompt's row bookkeeping needs a
// conservative width instead of treating every unknown character as one
// cell (CJK and emoji are two). These are the ranges that matter for text a
// user can type or that the app itself prints.
bool isZeroWidth(wchar_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||  // combining diacritics
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||  // variation selectors
           (cp >= 0xFE20 && cp <= 0xFE2F) ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF) ||  // emoji skin tones
           cp == 0x200D;                        // zero-width joiner
}

bool isWideFallback(wchar_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||  // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0xA4CF) ||  // CJK
           (cp >= 0xAC00 && cp <= 0xD7A3) ||  // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK compatibility
           (cp >= 0xFE30 && cp <= 0xFE4F) ||  // CJK compatibility forms
           (cp >= 0xFF00 && cp <= 0xFF60) ||  // fullwidth forms
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F000 && cp <= 0x1FAFF) ||  // emoji
           (cp >= 0x20000 && cp <= 0x3FFFD);    // CJK extension B+
}

// Emoji that Unicode marks as East Asian Wide but some wcwidth() tables still
// report as one cell (glibc varies by locale, for example for U+23F3).
bool isAlwaysWideEmoji(wchar_t cp) {
    return (cp >= 0x231A && cp <= 0x231B) ||
           (cp >= 0x23E9 && cp <= 0x23EC) || cp == 0x23F0 || cp == 0x23F3 ||
           (cp >= 0x25FD && cp <= 0x25FE) ||
           (cp >= 0x2614 && cp <= 0x2615) ||
           (cp >= 0x2648 && cp <= 0x2653) || cp == 0x267F || cp == 0x2693 ||
           cp == 0x26A1 || (cp >= 0x26AA && cp <= 0x26AB) ||
           (cp >= 0x26BD && cp <= 0x26BE) ||
           (cp >= 0x26C4 && cp <= 0x26C5) || cp == 0x26CE || cp == 0x26D4 ||
           cp == 0x26EA || (cp >= 0x26F2 && cp <= 0x26F3) || cp == 0x26F5 ||
           cp == 0x26FA || cp == 0x26FD || cp == 0x2705 ||
           (cp >= 0x270A && cp <= 0x270B) || cp == 0x2728 || cp == 0x274C ||
           cp == 0x274E || (cp >= 0x2753 && cp <= 0x2755) || cp == 0x2757 ||
           (cp >= 0x2795 && cp <= 0x2797) || cp == 0x27B0 || cp == 0x27BF ||
           (cp >= 0x2B1B && cp <= 0x2B1C) || cp == 0x2B50 || cp == 0x2B55 ||
           (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||  // regional indicators
           (cp >= 0x1F300 && cp <= 0x1F9FF) ||
           (cp >= 0x1FA70 && cp <= 0x1FAFF);
}

// The width rules shared by displayWidth() and the input-area row splitter.
struct WidthState {
    int width = 0;
    int lastWidth = 0;
    bool joinNext = false;      // the previous codepoint was a ZWJ
    bool lastRegional = false;  // the previous codepoint was a flag half

    void feed(wchar_t cp) {
        // VS16 asks the terminal to draw the preceding character as an
        // emoji, which is two cells wide even when wcwidth() reports the
        // base as narrow (for example U+2744 snowflake).
        if (cp == 0xFE0F) {
            if (lastWidth < 2) width += 2 - lastWidth;
            lastWidth = 2;
            return;
        }
        if (cp == 0xFE0E) return;  // VS15: force text presentation
        if (cp == 0x200D) {        // ZWJ: the next emoji joins the cluster
            joinNext = true;
            lastWidth = 0;
            return;
        }
        if (isZeroWidth(cp)) return;  // combining marks, skin tones

        int cw = ::wcwidth(cp);
        if (cw < 0) cw = isWideFallback(cp) ? 2 : 1;
        if (cw < 2 && isAlwaysWideEmoji(cp)) cw = 2;

        const bool regional = cp >= 0x1F1E6 && cp <= 0x1F1FF;
        if (joinNext || (regional && lastRegional)) {
            // A joined emoji and the second half of a flag add no width:
            // the cluster is already counted as two cells.
            joinNext = false;
            lastRegional = false;
            lastWidth = 2;
            return;
        }
        width += cw;
        lastWidth = cw;
        lastRegional = regional;
    }
};

// Columns a rendered line takes, ignoring ANSI sequences.
int displayWidth(const std::string& s) {
    WidthState st;
    for (size_t i = 0; i < s.size();) {
        if (static_cast<unsigned char>(s[i]) == 0x1b && i + 1 < s.size() &&
            s[i + 1] == '[') {
            i += 2;
            while (i < s.size()) {
                unsigned char u = static_cast<unsigned char>(s[i++]);
                if (u >= 0x40 && u <= 0x7e) break;
            }
            continue;
        }
        wchar_t cp = 0;
        i += decodeUtf8(s, i, cp);
        st.feed(cp);
    }
    return st.width;
}

// Rows a rendered line occupies. A line that ends exactly at the right
// margin leaves the cursor on the last column, so a full row is still one
// row, not two.
int rowsForWidth(int width, int cols) {
    return width < 1 ? 1 : (width + cols - 1) / cols;
}

// One terminal row of the input body. A long hard line is split into several
// DisplayRows.
struct DisplayRow {
    size_t start = 0;
    size_t end = 0;
    int width = 0;
};

// Splits `buf` into terminal rows of at most `cols` columns. Newlines force a
// new row. The caller decides which slice of rows to show.
std::vector<DisplayRow> buildDisplayRows(const std::string& buf, int cols) {
    std::vector<DisplayRow> rows;
    size_t lineStart = 0;
    for (;;) {
        const size_t nl = buf.find('\n', lineStart);
        const bool hard = nl != std::string::npos;
        const size_t lineEnd = hard ? nl : buf.size();

        size_t rowStart = lineStart;
        while (rowStart < lineEnd) {
            WidthState st;
            size_t j = rowStart;
            size_t rowEnd = rowStart;
            int rowWidth = 0;
            while (j < lineEnd) {
                wchar_t cp = 0;
                const size_t len = decodeUtf8(buf, j, cp);
                WidthState next = st;
                next.feed(cp);
                if (next.width > cols && st.width > 0) break;
                st = next;
                j += len;
                rowEnd = j;
                rowWidth = st.width;
                if (st.width >= cols) break;
            }
            if (rowEnd == rowStart) {
                // A single codepoint wider than the terminal: keep it on its
                // own row instead of looping forever.
                wchar_t cp = 0;
                rowEnd = rowStart + decodeUtf8(buf, rowStart, cp);
                rowWidth =
                    displayWidth(buf.substr(rowStart, rowEnd - rowStart));
            }
            rows.push_back({rowStart, rowEnd, rowWidth});
            rowStart = rowEnd;
        }

        if (rowStart == lineStart) {
            // Empty line (including the empty line after a trailing newline).
            rows.push_back({lineStart, lineStart, 0});
        }

        if (!hard) break;
        lineStart = nl + 1;
    }
    return rows;
}

struct InputArea {
    std::string text;
    int rows = 1;      // header rows plus body rows
    int caretRow = 0;  // 0-based row of the caret inside the area
    int caretCol = 0;  // 0-based column of the caret inside its row
};

// Layout: the prompt/timer/status header is pinned at the top-left, and the
// input body scrolls below it. The body grows with the text up to roughly a
// third of the terminal (at most 8 rows), then scrolls to keep the caret
// visible.
InputArea buildInputArea(const std::string& prefix, const std::string& buf,
                         size_t caret, int cols, int termRows,
                         int& viewStart, int& bodyRowsState) {
    std::vector<DisplayRow> rows = buildDisplayRows(buf, cols);

    // Give an end-of-line caret its own row when the last row is exactly
    // full, so the next typed character wraps instead of overwriting.
    if (caret == buf.size() && rows.back().width >= cols)
        rows.push_back({buf.size(), buf.size(), 0});

    int caretRow = 0;
    int caretCol = 0;
    bool found = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        const DisplayRow& r = rows[i];
        if (caret < r.start || caret > r.end) continue;
        const int col =
            (caret == r.end)
                ? r.width
                : displayWidth(buf.substr(r.start, caret - r.start));
        if (caret == r.end && col >= cols && i + 1 < rows.size()) {
            caretRow = static_cast<int>(i) + 1;
            caretCol = 0;
        } else {
            caretRow = static_cast<int>(i);
            caretCol = col;
        }
        found = true;
        break;
    }
    if (!found) {
        caretRow = static_cast<int>(rows.size()) - 1;
        caretCol = 0;
    }

    // Reserve the header first, then size the body from what is left.
    const int headerRows = rowsForWidth(displayWidth(prefix), cols);
    const int userCap = std::min(8, std::max(1, termRows / 3));
    const int bodyCap =
        std::min(userCap, std::max(1, termRows - headerRows - 1));
    const int totalRows = static_cast<int>(rows.size());
    int bodyRows = std::min(totalRows, bodyCap);

    // Let the area grow with the text, but keep its height for the rest of
    // this prompt: shrinking it would leave the footer floating above blank
    // terminal rows. A resize can still force it smaller.
    if (bodyRows < bodyRowsState) bodyRows = std::min(bodyRowsState, bodyCap);
    bodyRowsState = bodyRows;

    // Keep the viewport where it was while the caret stays inside it; only
    // scroll when the caret leaves. Clamp after the content or area resized.
    const int maxStart = std::max(0, totalRows - bodyRows);
    if (viewStart < 0) viewStart = 0;
    if (viewStart > maxStart) viewStart = maxStart;
    if (caretRow < viewStart) viewStart = caretRow;
    if (caretRow >= viewStart + bodyRows)
        viewStart = caretRow - bodyRows + 1;
    if (viewStart < 0) viewStart = 0;
    if (viewStart > maxStart) viewStart = maxStart;

    InputArea area;
    area.rows = headerRows + bodyRows;
    area.caretRow = headerRows + (caretRow - viewStart);
    area.caretCol = caretCol;

    area.text = prefix;
    area.text += '\n';
    for (int i = viewStart; i < viewStart + bodyRows; ++i) {
        if (i > viewStart) area.text += '\n';
        if (i < totalRows) {
            const DisplayRow& r = rows[static_cast<size_t>(i)];
            area.text.append(buf, r.start, r.end - r.start);
        }
    }
    return area;
}

std::string promptPrefix(bool timed, bool paused, long long remainingMs,
                         bool background, const std::string& status) {
    std::string s;
    if (timed) {
        long left = static_cast<long>((remainingMs + 999) / 1000);
        if (left < 1) left = 1; // still open, so never show 0s
        char sec[32];
        std::snprintf(sec, sizeof sec, "%s %2lds", paused ? "❄️" : "⏳",
                      left);
        s += ui::dim();
        s += sec;
        s += ui::off();
    } else if (!background) {
        s += ui::dim();
        s += "😴 Respond anytime";
        s += ui::off();
    } else {
        s += ui::softBlue();
        s += "🤖 ";
        s += status.empty() ? "autonomous work" : status;
        s += " — type to interrupt";
        s += ui::off();
    }
    s += timed ? "  " : " ";
    s += ui::userPrompt();
    return s;
}

// Repaints the input area in place. Any rows from the previous paint are
// erased first, so the body can grow or scroll without moving the transcript.
// The status header is painted first; the cursor is then moved back down to
// the caret in the body.
class Painter {
public:
    void paint(const InputArea& area) {
        moveToStart();
        std::printf("\r\33[J");

        // Print hard newlines as CRLF so the row bookkeeping does not depend
        // on the terminal's output post-processing.
        size_t start = 0;
        for (;;) {
            const size_t nl = area.text.find('\n', start);
            if (nl == std::string::npos) {
                std::fwrite(area.text.data() + start, 1,
                            area.text.size() - start, stdout);
                break;
            }
            std::fwrite(area.text.data() + start, 1, nl - start, stdout);
            std::fputs("\r\n", stdout);
            start = nl + 1;
        }

        const int endRow = area.rows - 1;
        if (area.caretRow < endRow)
            std::printf("\33[%dA", endRow - area.caretRow);
        std::printf("\r");
        if (area.caretCol > 0) std::printf("\33[%dC", area.caretCol);
        cursorRow_ = area.caretRow;
        std::fflush(stdout);
    }

    void clear() {
        moveToStart();
        std::printf("\r\33[J");
        std::fflush(stdout);
        cursorRow_ = 0;
    }

private:
    void moveToStart() {
        if (cursorRow_ > 0) std::printf("\33[%dA", cursorRow_);
    }

    int cursorRow_ = 0; // 0-based row the caret was left on
};

void clearLine() {
    std::printf("\r\33[K");
    std::fflush(stdout);
}

} // namespace

bool endsWithThinkMarker(const std::string& message) {
    static const char kMarker[] = "@action-think";
    constexpr size_t kMarkerLen = sizeof kMarker - 1;

    size_t end = message.size();
    while (end > 0 && (message[end - 1] == ' ' || message[end - 1] == '\t'))
        --end;
    if (end < kMarkerLen) return false;

    const size_t start = end - kMarkerLen;
    if (message.compare(start, kMarkerLen, kMarker) != 0) return false;

    // Only a standalone marker counts; "note@action-think" is just text.
    return start == 0 || message[start - 1] == ' ' ||
           message[start - 1] == '\t';
}

bool takeEnergyPotionMarker(std::string& message) {
    static const char kMarker[] = "@action-energy-potion";
    constexpr size_t kMarkerLen = sizeof kMarker - 1;

    auto boundary = [](char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return !std::isalnum(u) && c != '_' && c != '-' && c != '@';
    };
    auto space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };

    bool found = false;
    size_t from = 0;
    for (;;) {
        const size_t pos = message.find(kMarker, from);
        if (pos == std::string::npos) break;

        const size_t end = pos + kMarkerLen;
        const bool left = pos == 0 || boundary(message[pos - 1]);
        const bool right = end >= message.size() || boundary(message[end]);
        if (!left || !right) {
            from = pos + 1;
            continue;
        }

        // Take the whitespace on both sides so "hello @action-energy-potion
        // world" becomes "hello world", not "hello  world".
        size_t start = pos;
        while (start > 0 && space(message[start - 1])) --start;
        size_t after = end;
        while (after < message.size() && space(message[after])) ++after;

        if (start > 0 && after < message.size()) {
            const char next = message[after];
            if (std::ispunct(static_cast<unsigned char>(next)) && next != '@')
                message.erase(start, after - start);
            else
                message.replace(start, after - start, " ");
        } else {
            message.erase(start, after - start);
        }

        found = true;
        from = start;
    }

    if (found) {
        size_t begin = 0;
        while (begin < message.size() && space(message[begin])) ++begin;
        size_t end = message.size();
        while (end > begin && space(message[end - 1])) --end;
        if (begin > 0 || end < message.size())
            message = message.substr(begin, end - begin);
    }
    return found;
}

// One line, read straight from the file descriptor in canonical mode, so it
// can coexist with the raw-mode readers above (std::cin would keep bytes in
// its own buffer where read() cannot see them).
bool readLine(std::string& out) {
    out.clear();
    for (;;) {
        size_t nl = g_queued.find('\n');
        if (nl != std::string::npos) {
            out = g_queued.substr(0, nl);
            g_queued.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }
        char buf[256];
        ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            out = g_queued;
            g_queued.clear();
            return false;
        }
        if (n == 0) {
            out = g_queued;
            g_queued.clear();
            return !out.empty();
        }
        g_queued.append(buf, static_cast<size_t>(n));
    }
}

Turn readTurn(int timeoutSec, std::string& line,
              const std::function<bool()>& tick,
              const std::function<std::string()>& liveStatus) {
    line.clear();
    const bool watch = static_cast<bool>(tick);
    auto statusText = [&]() -> std::string {
        return watch && liveStatus ? liveStatus() : std::string();
    };

    // Pipes and files: no countdown to animate, plain read.
    if (!::isatty(STDIN_FILENO) || !enterRaw()) {
        if (watch && !tick()) return Turn::Background;
        if (watch) {
            const std::string status = statusText();
            std::printf("%s🤖 %s — type to interrupt%s %s", ui::softBlue(),
                        status.empty() ? "autonomous work" : status.c_str(),
                        ui::off(), ui::userPrompt().c_str());
        }
        else if (timeoutSec <= 0)
            std::printf("%s😴 Respond anytime%s %s", ui::dim(), ui::off(),
                        ui::userPrompt().c_str());
        else
            std::printf("%s⏳ %2ds%s  %s", ui::dim(), timeoutSec, ui::off(),
                        ui::userPrompt().c_str());
        std::fflush(stdout);
        while (watch) {
            pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int rc = ::poll(&pfd, 1, 1000);
            if (rc == 0) {
                if (!tick()) return Turn::Background;
                continue;
            }
            if (rc < 0 && errno == EINTR) continue;
            break;
        }
        if (!readLine(line)) return Turn::Eof;
        clearLine();
        return Turn::Line;
    }

    // The countdown pauses while the user is typing or editing and resumes
    // from the same remaining time after a short idle period. The timer
    // exists to nudge a silent user, not to interrupt someone mid-answer.
    constexpr long long kPauseIdleMs = 1000;
    const bool hasTimer = timeoutSec > 0;
    long long remainingMs =
        hasTimer ? static_cast<long long>(timeoutSec) * 1000 : 0;
    long long deadline = nowMs() + remainingMs;
    long long lastActivity = 0;
    bool paused = false;
    bool backgroundFinished = false;
    long long lastTickMs = nowMs();
    std::string buf;
    size_t caret = 0; // byte offset into buf, always on a UTF-8 boundary
    int viewStart = 0;     // first visible body row; kept across repaints
    int bodyRowsState = 0; // grow-only body height for this prompt
    Painter painter;

    auto redraw = [&] {
        long long left =
            paused ? remainingMs : (hasTimer ? deadline - nowMs() : 0);
        const std::string prefix =
            promptPrefix(hasTimer, paused, left, watch, statusText());
        painter.paint(buildInputArea(prefix, buf, caret, termCols(),
                                     termRows(), viewStart, bodyRowsState));
    };
    redraw();

    for (;;) {
        long long now = nowMs();

        // Resume after one second without a keystroke, keeping the time that
        // was left when typing started. While the message ends with
        // "@action-think" the user says they are still thinking, so the
        // countdown stays paused for as long as the marker is there.
        const bool thinking = endsWithThinkMarker(buf);
        if (hasTimer && paused && !thinking &&
            now - lastActivity >= kPauseIdleMs) {
            paused = false;
            deadline = now + remainingMs;
            redraw();
        }

        if (hasTimer && !paused) {
            remainingMs = deadline - now;
            if (remainingMs <= 0) {
                // A keystroke that arrived just before the deadline still
                // wins: it pauses the timer.
                bool inputReady = !g_queued.empty();
                if (!inputReady) {
                    pollfd pfd;
                    pfd.fd = STDIN_FILENO;
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    inputReady = ::poll(&pfd, 1, 0) > 0;
                }
                if (!inputReady) {
                    leaveRaw();
                    painter.clear();
                    return Turn::Timeout;
                }
            }
        }

        if (g_queued.empty()) {
            int waitMs;
            if (!hasTimer && !watch) {
                waitMs = -1;
            } else if (!hasTimer) {
                // Repaint often enough to animate the background spinner;
                // the worker tick itself still runs once per second below.
                waitMs = backgroundFinished ? 1000 : 100;
            } else if (paused) {
                if (thinking) {
                    // No idle timeout: wait for the next keystroke, but keep
                    // ticking if the background worker needs attention.
                    waitMs = watch ? 100 : -1;
                } else {
                    long long idleLeft =
                        kPauseIdleMs - (nowMs() - lastActivity);
                    if (idleLeft < 0) idleLeft = 0;
                    waitMs = static_cast<int>(std::min<long long>(
                        idleLeft, watch ? 100 : 1000));
                }
            } else if (remainingMs <= 0) {
                waitMs = 0; // read the late keystroke
            } else {
                const long long quantum = watch ? 100 : 1000;
                waitMs = static_cast<int>(
                    std::min<long long>(remainingMs, quantum));
            }
            pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int rc = ::poll(&pfd, 1, waitMs);
            if (rc < 0) {
                if (errno == EINTR) continue;
                leaveRaw();
                painter.clear();
                return Turn::Eof;
            }
            if (rc == 0) { // poll timed out: repaint, keeping what was typed
                const long long tickNow = nowMs();
                if (watch && !backgroundFinished &&
                    tickNow - lastTickMs >= 1000) {
                    lastTickMs = tickNow;
                    // Clear the prompt first so the tick can print status
                    // lines from the background worker without corrupting
                    // the painter's row bookkeeping.
                    painter.clear();
                    if (!tick()) {
                        // Do not throw away a half-written message just
                        // because the worker finished. Keep reading until
                        // the user submits it (the caller applies the
                        // finished result before handling the line).
                        if (buf.empty()) {
                            leaveRaw();
                            return Turn::Background;
                        }
                        backgroundFinished = true;
                    }
                }
                redraw();
                continue;
            }
        }

        char c = 0;
        if (!readByte(-1, c)) {
            leaveRaw();
            painter.clear();
            return Turn::Eof;
        }

        if (c == '\n' || c == '\r') {
            leaveRaw();
            painter.clear();
            line = buf;
            return Turn::Line;
        }

        if (c == 0x04) { // Ctrl-D: EOF on an empty line, else submit as-is
            leaveRaw();
            painter.clear();
            if (buf.empty()) return Turn::Eof;
            line = buf;
            return Turn::Line;
        }

        bool changed = false;
        if (c == 0x7f || c == 0x08) { // Backspace
            changed = eraseBeforeCaret(buf, caret);
        } else if (c == 0x15) { // Ctrl-U: kill from the start to the caret
            changed = caret > 0;
            buf.erase(0, caret);
            caret = 0;
        } else if (c == 0x17) { // Ctrl-W: erase the word before the caret
            changed = eraseWordBeforeCaret(buf, caret);
        } else if (c == 0x1b) { // Escape sequence
            const size_t before = caret;
            switch (readEscape()) {
            case Key::Left:
                if (caret > 0) caret = prevCodePoint(buf, caret);
                break;
            case Key::Right:
                if (caret < buf.size()) caret = nextCodePoint(buf, caret);
                break;
            case Key::Home:
                caret = 0;
                break;
            case Key::End:
                caret = buf.size();
                break;
            case Key::Delete:
                changed = eraseAtCaret(buf, caret);
                break;
            case Key::PasteStart: {
                std::string pasted;
                if (readPaste(pasted)) {
                    const std::string clean = cleanPaste(pasted);
                    if (!clean.empty()) {
                        buf.insert(caret, clean);
                        caret += clean.size();
                        changed = true;
                    }
                }
                break;
            }
            case Key::PasteEnd:
                break;
            case Key::None:
                break;
            }
            changed = changed || caret != before;
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            buf.insert(caret, 1, c);
            ++caret;
            changed = true;
        }
        if (changed) {
            if (hasTimer && !paused) {
                remainingMs = std::max<long long>(0, deadline - nowMs());
                paused = true;
            }
            if (hasTimer) lastActivity = nowMs();
            redraw();
        }
    }
}

bool readSecret(std::string& out) {
    out.clear();

    if (!::isatty(STDIN_FILENO) || !enterRaw()) {
        return readLine(out);
    }

    std::string buf;
    for (;;) {
        char c = 0;
        if (!readByte(-1, c)) {
            leaveRaw();
            std::printf("\n");
            std::fflush(stdout);
            return false;
        }

        if (c == '\n' || c == '\r') {
            leaveRaw();
            std::printf("\n");
            std::fflush(stdout);
            out = buf;
            return true;
        }

        if (c == 0x04) { // Ctrl-D: EOF on an empty line, else submit as-is
            leaveRaw();
            std::printf("\n");
            std::fflush(stdout);
            if (buf.empty()) return false;
            out = buf;
            return true;
        }

        size_t before = buf.size();
        if (c == 0x7f || c == 0x08) { // Backspace
            eraseLastChar(buf);
        } else if (c == 0x15) { // Ctrl-U
            buf.clear();
        } else if (c == 0x17) { // Ctrl-W
            eraseLastWord(buf);
        } else if (c == 0x1b) { // Escape sequence
            (void)readEscape();
            continue;
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            buf += c;
        } else {
            continue;
        }

        // One placeholder per byte kept, so the row always matches the mask.
        size_t after = buf.size();
        for (size_t i = after; i < before; ++i) std::printf("\b \b");
        for (size_t i = before; i < after; ++i) std::putchar('*');
        std::fflush(stdout);
    }
}
