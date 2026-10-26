// ai-chat — the dialogue loop.
//
//   * Reads one turn (with a countdown), handling /commands.
//   * Streams the model's reply through the API client.
//   * Lets the model choose the next countdown (via <timer N/>) and turn it
//     off (via <wait/>) when you explicitly ask it to wait, say you need a
//     long time, or after a timeout. An exact time from the user may also
//     allow a timer longer than the normal 30-180 second range.
//   * Runs an autonomous background loop (via <auto/> or /auto) that works on
//     the short-term target in the self-cognition file while the chat stays
//     interruptible.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

#include "aichat/core/autonomy.h"
#include "aichat/core/chat.h"
#include "aichat/app/cli.h"
#include "aichat/core/cognition.h"
#include "aichat/core/config.h"
#include "aichat/core/documentary.h"
#include "aichat/core/experience.h"
#include "aichat/core/story.h"
#include "aichat/core/identity.h"
#include "aichat/io/input.h"
#include "aichat/tests/selftest.h"
#include "aichat/ui/ui.h"
#include "aichat/core/util.h"
#include "ai-tools/local-dir-access/local-dir-access.h"
#include "ai-tools/sandbox-mirror/sandbox-mirror.h"
#include "ai-tools/self-made-tool/self-made-tool.h"
#include "ai-tools/text-utf8/text-utf8.h"
#include "aichat/core/tag_manuals.h"

namespace {

bool hasWord(const std::string& text, const std::string& word) {
    for (size_t pos = text.find(word); pos != std::string::npos;
         pos = text.find(word, pos + word.size())) {
        const bool left =
            pos == 0 ||
            !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
        const size_t end = pos + word.size();
        const bool right =
            end >= text.size() ||
            !std::isalnum(static_cast<unsigned char>(text[end]));
        if (left && right) return true;
    }
    return false;
}

// Session-control lines are green, except failures (red).
void sessionNote(const std::string& text) {
    const bool problem = text.find("could not") != std::string::npos ||
                         text.find("stopping") != std::string::npos ||
                         hasWord(text, "no");
    ui::noteParts({{text, problem ? ui::red() : ui::green()}});
}

// A single live line for a blocking model call (documentary merge, memory
// record, self-cognition refresh). It proves the process is alive and shows
// the token estimate growing; it is removed when the call returns.
class WaitTicker {
  public:
    explicit WaitTicker(std::string label) : label_(std::move(label)) {}
    ~WaitTicker() { stop(); }

    void start() {
        if (!::isatty(STDOUT_FILENO)) return;
        running_.store(true);
        thread_ = std::thread([this] { loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        std::fputs("\r\033[K", stdout);
        std::fflush(stdout);
    }

  private:
    static long long nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
                   steady_clock::now().time_since_epoch())
            .count();
    }

    void loop() {
        static const char* const frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                            "⠴", "⠦", "⠧", "⠇", "⠏"};
        constexpr int frameCount =
            static_cast<int>(sizeof(frames) / sizeof(frames[0]));
        int frame = 0;
        const auto began = std::chrono::steady_clock::now();
        while (running_.load()) {
            const long long elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - began)
                    .count();
            const bool active = completionActive();
            const long long seen = completionTokensSeen();
            const long long last = completionLastActivityMs();
            long long silent = 0;
            if (active && last > 0) silent = (nowMs() - last) / 1000;

            std::string state;
            if (!active)
                state = "waiting";
            else if (silent >= 15)
                state = "silent " + std::to_string(silent) + "s";
            else if (seen <= 0)
                state = elapsed < 5 ? "connecting" : "connected · healthy";
            else
                state = "streaming · healthy";

            std::ostringstream line;
            line << "\r\033[K" << frames[frame++ % frameCount] << " " << label_
                 << " · " << elapsed << "s · " << state;
            if (seen > 0) line << " · tokens ~" << seen;
            std::fputs(line.str().c_str(), stdout);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::string label_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Autonomous worker transcript
//
// Worker messages are rendered as a quiet, indented tree. Lifecycle lines get
// a "*" bullet, fields are aligned, and values that are likely to be long
// start on their own line. Cycle, result and energy therefore stay easy to
// scan without a wall of color.
// ---------------------------------------------------------------------------

constexpr size_t kWorkerLabelWidth = 10;

std::string workerLabel(const std::string& label) {
    std::string padded = label;
    if (padded.size() < kWorkerLabelWidth)
        padded.append(kWorkerLabelWidth - padded.size(), ' ');
    return padded + " ";
}

size_t transcriptColumns() {
    winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

// True when a value should begin on the next line: it is multi-line, or it
// would not fit beside its label in the current terminal.
bool needsOwnLine(const std::string& value, size_t usedColumns) {
    return value.find('\n') != std::string::npos ||
           value.find('\r') != std::string::npos ||
           usedColumns + value.size() > transcriptColumns();
}

// Prints one value, one source line per transcript row.
void printValueRows(const std::string& value, const std::string& indent,
                    const char* color) {
    size_t start = 0;
    for (;;) {
        const size_t nl = value.find('\n', start);
        const size_t len =
            nl == std::string::npos ? std::string::npos : nl - start;
        const std::string row = value.substr(start, len);
        if (!row.empty()) ui::noteParts({{row, color}}, indent);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// An indented label/value pair. A long or multi-line value begins on the next
// line so the label does not get pushed around by the terminal.
void printField(const std::string& label, const std::string& value,
                const char* labelColor = ui::dim(),
                const char* valueColor = ui::brightGray(),
                const std::string& indent = "    ",
                const std::string& valueIndent = "      ") {
    const std::string labelText = workerLabel(label);
    if (!needsOwnLine(value, indent.size() + labelText.size())) {
        ui::noteParts({{labelText, labelColor}, {value, valueColor}}, indent);
        return;
    }
    ui::noteParts({{label, labelColor}}, indent);
    printValueRows(value, valueIndent, valueColor);
}

// A label whose value always starts on its own line.
void printFieldBlock(const std::string& label, const std::string& value,
                     const char* labelColor = ui::dim(),
                     const char* valueColor = ui::brightGray(),
                     const std::string& indent = "    ",
                     const std::string& valueIndent = "      ") {
    ui::noteParts({{label, labelColor}}, indent);
    printValueRows(value, valueIndent, valueColor);
}

void printWorkerPlain(const std::string& text, const char* color) {
    ui::noteParts({{text, color}});
}

const char* fallbackTone(const std::string& line) {
    if (line.find("failed") != std::string::npos ||
        line.find("failing") != std::string::npos ||
        line.find("error") != std::string::npos ||
        line.find("empty repl") != std::string::npos ||
        line.find("kept returning") != std::string::npos ||
        line.find("not updated") != std::string::npos ||
        startsWith(line, "could not"))
        return ui::red();
    if (line.find("refused") != std::string::npos ||
        line.find("rejected") != std::string::npos ||
        line.find("gave up") != std::string::npos ||
        line.find("tired") != std::string::npos ||
        startsWith(line, "out of energy"))
        return ui::yellow();
    if (line.find("succeeded") != std::string::npos ||
        line.find("approved") != std::string::npos ||
        line.find("updated") != std::string::npos)
        return ui::green();
    return ui::brightGray();
}

std::string attributeAfter(const std::string& text, const std::string& key) {
    size_t pos = text.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    const size_t end = text.find(',', pos);
    return trim(text.substr(pos, end == std::string::npos
                                     ? std::string::npos
                                     : end - pos));
}

void printCycleHeader(const std::string& line) {
    const size_t colon = line.find(": ");
    if (colon == std::string::npos) {
        printWorkerPlain(line, ui::brightGray());
        return;
    }
    const std::string cycle = line.substr(6, colon - 6);
    const std::string rest = line.substr(colon + 2);
    std::string phase = rest;
    std::string cost;
    std::string energy;
    const size_t open = rest.find(" (");
    const size_t close = rest.rfind(')');
    if (open != std::string::npos && close != std::string::npos &&
        close > open) {
        phase = rest.substr(0, open);
        const std::string attrs = rest.substr(open + 2, close - open - 2);
        cost = attributeAfter(attrs, "cost ");
        energy = attributeAfter(attrs, "energy ");
    }

    std::vector<ui::TextPart> parts;
    parts.push_back({"cycle ", ui::dim()});
    parts.push_back({cycle, ui::bold()});
    // Every cycle is logged when it enters planning, so repeating that word in
    // every header is noise. Other phases are still shown if a log line ever
    // carries one; preview/action/observation have their own fields below.
    if (!phase.empty() && phase != "planning") {
        parts.push_back({" · ", ui::dim()});
        parts.push_back({phase, ui::brightGray()});
    }
    if (!energy.empty()) {
        parts.push_back({" · ", ui::dim()});
        parts.push_back({"energy ", ui::dim()});
        parts.push_back({energy, ui::purple()});
    }
    if (!cost.empty()) {
        parts.push_back({" · ", ui::dim()});
        parts.push_back({"cost ", ui::dim()});
        parts.push_back({cost, ui::purple()});
    }
    ui::noteParts(parts);
}

void printWorkerNote(const std::string& line) {
    if (startsWith(line, "autonomous loop started")) {
        std::vector<ui::TextPart> parts;
        parts.push_back({"autonomous loop started", ui::cyan()});
        const size_t at = line.find("energy ");
        if (at != std::string::npos) {
            std::string energy = line.substr(at + 7);
            if (!energy.empty() && energy.back() == ')') energy.pop_back();
            parts.push_back({" · ", ui::dim()});
            parts.push_back({"energy ", ui::dim()});
            parts.push_back({energy, ui::purple()});
        }
        ui::noteParts(parts);
        return;
    }

    if (line == "analysing the short-term target…") {
        printWorkerPlain(line, ui::brightGray());
        return;
    }

    if (startsWith(line, "target: ")) {
        printFieldBlock("target", line.substr(8), ui::cyan());
        return;
    }

    if (startsWith(line, "cycle ")) {
        std::printf("\n");
        printCycleHeader(line);
        return;
    }

    if (startsWith(line, "cost: ")) {
        printField("cost", line.substr(6), ui::dim(), ui::purple());
        return;
    }

    if (startsWith(line, "proposed: ")) {
        printField("proposed", line.substr(10));
        return;
    }

    if (line == "preview approved the step") {
        printField("preview", "approved", ui::dim(), ui::green());
        return;
    }

    if (startsWith(line, "preview rejected the step: ")) {
        printField("preview", "rejected", ui::dim(), ui::yellow());
        printField("reason", line.substr(27));
        return;
    }

    if (line == "preview gave no usable verdict; the step was not run") {
        printField("preview", "no usable verdict; step not run", ui::dim(),
                   ui::yellow());
        return;
    }

    if (startsWith(line, "guard refused ")) {
        const size_t colon = line.find(": ");
        const std::string what =
            colon == std::string::npos
                ? line.substr(14)
                : line.substr(14, colon - 14);
        printField("guard", "refused " + what, ui::dim(), ui::yellow());
        if (colon != std::string::npos)
            printField("reason", line.substr(colon + 2));
        return;
    }

    if (startsWith(line, "action ")) {
        const std::string rest = line.substr(7);
        const std::string okSep = " succeeded: ";
        const std::string failSep = " failed: ";
        size_t sep = rest.find(okSep);
        const bool ok = sep != std::string::npos;
        if (!ok) sep = rest.find(failSep);
        const std::string name =
            sep == std::string::npos ? rest : rest.substr(0, sep);
        std::vector<ui::TextPart> parts;
        parts.push_back({workerLabel("action"), ui::dim()});
        parts.push_back({name, ui::brightGray()});
        parts.push_back({" · ", ui::dim()});
        parts.push_back({ok ? "succeeded" : "failed",
                         ok ? ui::green() : ui::red()});
        ui::noteParts(parts, "    ");
        if (sep != std::string::npos) {
            const size_t start = sep + (ok ? okSep.size() : failSep.size());
            printField("result", rest.substr(start), ui::dim(),
                       ok ? ui::brightGray() : ui::red());
        }
        return;
    }

    if (startsWith(line, "result: ")) {
        printField("result", line.substr(8));
        return;
    }

    if (startsWith(line, "observation: ")) {
        printFieldBlock("observation", line.substr(13));
        return;
    }

    if (startsWith(line, "observation unavailable")) {
        printWorkerPlain(line, ui::yellow());
        return;
    }

    if (line == "observation produced no decision; continuing") {
        printWorkerPlain(line, ui::dim());
        return;
    }

    if (startsWith(line, "motivation: ")) {
        printField("motivation", line.substr(12));
        return;
    }

    if (startsWith(line, "experience saved to ")) {
        printField("experience", "saved to " + line.substr(19), ui::dim(),
                   ui::dim());
        return;
    }

    if (line == "experience found for this target") {
        printWorkerPlain(line, ui::dim());
        return;
    }

    if (line == "self-cognition updated") {
        printWorkerPlain(line, ui::green());
        return;
    }

    if (startsWith(line, "self-cognition changed while paused")) {
        printWorkerPlain(line, ui::brightGray());
        return;
    }

    if (startsWith(line, "switched to this target's own motivation")) {
        printWorkerPlain(line, ui::dim());
        return;
    }

    if (startsWith(line, "giving up: ")) {
        printField("giving up", line.substr(11), ui::dim(), ui::yellow());
        return;
    }

    if (startsWith(line, "no short-term target")) {
        printWorkerPlain(line, ui::yellow());
        return;
    }

    if (startsWith(line, "out of energy")) {
        printWorkerPlain(line, ui::yellow());
        return;
    }

    if (startsWith(line, "planner reports the target is done")) {
        printWorkerPlain(line, ui::green());
        return;
    }

    if (startsWith(line, "could not re-check the target")) {
        printWorkerPlain(line, ui::yellow());
        return;
    }

    // "stage failed (reason)" from recordFailure().
    if (line.find(" failed (") != std::string::npos) {
        const size_t at = line.find(" failed (");
        const std::string stage = line.substr(0, at);
        std::string why = line.substr(at + 9);
        if (!why.empty() && why.back() == ')') why.pop_back();
        printField(stage, "failed", ui::dim(), ui::red());
        if (!why.empty()) printField("reason", why);
        return;
    }

    // Unknown message: keep a short label on the left when there is one, and
    // let long text take its own line.
    const size_t colon = line.find(": ");
    if (colon != std::string::npos && colon < 30) {
        const char* tone = fallbackTone(line.substr(colon + 2));
        printField(line.substr(0, colon), line.substr(colon + 2), ui::dim(),
                   tone);
        return;
    }
    printWorkerPlain(line, fallbackTone(line));
}

// End-of-session panel: orange lifecycle line, then compact fields. The
// target and reflection always start on their own line because they can be
// long; short fields stay beside their labels.
void printEndHeader(const std::string& state, const char* stateColor,
                    int steps, double energyLeft) {
    std::vector<ui::TextPart> parts;
    parts.push_back({"autonomous loop ", ui::orange()});
    parts.push_back({state, stateColor});
    parts.push_back({" · cycles ", ui::dim()});
    parts.push_back({std::to_string(steps), ui::brightGray()});
    parts.push_back({" · energy ", ui::dim()});
    parts.push_back({amountText(energyLeft) + "/" + amountText(1000.0),
                     ui::purple()});
    ui::noteParts(parts);
}

void printEndField(const std::string& label, const std::string& value,
                   const char* valueColor = ui::gray()) {
    printField(label, value, ui::orange(), valueColor, "  ", "    ");
}

void printEndBlock(const std::string& label, const std::string& value,
                   const char* valueColor = ui::gray()) {
    printFieldBlock(label, value, ui::orange(), valueColor, "  ", "    ");
}

// The /auto status panel: state, cycle and energy on the header, then only the
// fields that are present. A long target starts on its own line.
void printAutonomyStatus(const AutonomyStatus& s) {
    if (!s.active) {
        printWorkerPlain("no autonomous loop is active", ui::dim());
        return;
    }
    std::vector<ui::TextPart> parts;
    parts.push_back({"autonomous work", ui::cyan()});
    parts.push_back({" · ", ui::dim()});
    parts.push_back({s.state, s.state == "running" ? ui::green() : ui::yellow()});
    if (s.cycle > 0) {
        parts.push_back({" · cycle ", ui::dim()});
        parts.push_back({std::to_string(s.cycle), ui::bold()});
        parts.push_back({" · tokens ", ui::dim()});
        parts.push_back({std::to_string(s.tokens), ui::brightGray()});
    }
    parts.push_back({" · energy ", ui::dim()});
    parts.push_back({amountText(s.energy) + "/" +
                         amountText(s.energyFull),
                     ui::purple()});
    if (s.cycle > 0) {
        parts.push_back({" · cost ", ui::dim()});
        parts.push_back({amountText(s.cost), ui::purple()});
    }
    ui::noteParts(parts);
    if (!s.phase.empty() && s.phase != s.state)
        printField("phase", s.phase);
    if (!s.target.empty())
        printFieldBlock("target", s.target, ui::cyan());
    if (!s.last.empty()) printField("last", s.last);
}

// The dialogue loop: one turn in, one streamed reply out.
void chatLoop(Config& cfg, const std::string& tagManuals) {
    std::string cognitionText = cognition::load();
    // The last autonomous target gives a chat lesson a natural home. Before
    // any autonomous run, lessons go to the stable "chat" target instead of
    // guessing from the self-cognition prose.
    std::string lastExperienceTarget;
    std::string practiceDigest;
    std::string storyText;
    // The rendered documentary memory is the compacted/long-term part of the
    // system prompt. It is rebuilt after every raw item or merge.
    documentary::Memory documentaryMemory = documentary::load();
    std::string documentaryText = documentary::render(documentaryMemory, 8000);
    auto refreshPracticeDigest = [&] {
        practiceDigest.clear();
        std::ifstream in(experience::practicePath());
        if (!in) return;
        std::ostringstream out;
        out << in.rdbuf();
        practiceDigest = text_utf8::tail(out.str(), 4000);
    };
    refreshPracticeDigest();
    auto refreshStory = [&] {
        story::sweep();
        storyText = story::renderForPrompt(5000);
    };
    refreshStory();
    // The identity tracker is the durable, object-oriented world model. It is
    // separate from both the self-cognition narrative and the worker's task
    // state.
    identity::Tracker tracker = identity::load();
    // A login-id is a session label, not an identity kind. Binding it to a
    // tracked person is app-owned and makes the current user deterministic
    // even when several people share the same terminal. Test mode is
    // read-only, so an unknown login is only used in the prompt there.
    if (!ui::testMode()) {
        std::string loginNote;
        if (!identity::bindLogin(tracker, cfg.login_id, loginNote)) {
            ui::note(loginNote);
        } else if (!loginNote.empty()) {
            if (!identity::save(tracker))
                ui::note("could not save the login binding to the identity "
                         "tracker");
        }
    }
    std::string trackerFull = identity::render(tracker);
    // The chat prompt gets a relevance-selected view, not the whole tracker.
    // `viewFocus` is the app-owned anchor (normally the user object's name).
    std::string viewFocus;
    std::vector<std::string> manualFocus; // /focus override; empty = automatic
    std::string trackerText;
    // Start of the period that the next raw item will cover.
    std::string memorySegmentStart = documentary::now();
    std::vector<Message> history;
    bool testWarned = false; // one-time long-context note in test mode
    bool documentaryMergeFailed = false; // do not retry a failed merge every turn

    // The background worker only reads snapshots of this state. The main
    // thread is the only writer, and it only writes while the worker is
    // paused or finished; the mutex makes that contract explicit (and keeps
    // the worker from copying a half-updated history).
    std::mutex stateMutex;
    AutonomyController autonomy;

    // A short tail of the conversation used only to rank tracker objects.
    auto recentViewText = [&] {
        std::ostringstream out;
        const size_t start = history.size() > 5 ? history.size() - 5 : 0;
        for (size_t i = start; i < history.size(); ++i) {
            if (history[i].role == "system") continue;
            const std::string who =
                history[i].role == "user"
                    ? (cfg.login_id.empty() ? std::string("User")
                                            : cfg.login_id)
                    : cfg.nick;
            out << who << ": " << history[i].content << "\n";
        }
        std::string text = out.str();
        if (text.size() > 2000) {
            text.erase(0, text.size() - 2000);
            const size_t nl = text.find('\n');
            if (nl != std::string::npos) text.erase(0, nl + 1);
        }
        return text;
    };

    auto buildTrackerView = [&] {
        identity::ViewContext ctx;
        ctx.user_message = viewFocus;
        ctx.recent = recentViewText();
        ctx.task = autonomy.status().target;
        // The app, not the model, chooses the anchor. A manual /focus
        // override wins; otherwise chat anchors on the person bound to the
        // current login-id, falling back to the object whose canonical role
        // is "user" for legacy trackers.
        if (!manualFocus.empty()) {
            ctx.focus = manualFocus;
        } else if (const identity::Object* me =
                       identity::objectForLogin(tracker, cfg.login_id)) {
            ctx.focus.push_back(me->name);
        } else if (const identity::Object* user =
                       identity::primaryWithRole(tracker, "user")) {
            ctx.focus.push_back(user->name);
        }
        trackerText = identity::renderView(tracker, ctx).text;
    };
    buildTrackerView();

    auto systemText = [&] {
        return ui::systemPrompt(cfg.timeout, cfg.nick, cognitionText,
                                documentaryText, autonomy.statusText(),
                                tagManuals, trackerText, cfg.login_id,
                                practiceDigest, storyText);
    };
    auto resetSystem = [&] { history[0].content = systemText(); };

    auto recordChatLesson = [&](const std::string& body) {
        if (ui::testMode()) {
            ui::note("test mode: the lesson tag is read-only");
            return;
        }
        const std::string text = trim(body);
        if (text.empty()) return;
        const AutonomyStatus live = autonomy.status();
        std::string target = !live.target.empty() ? live.target
                                                  : lastExperienceTarget;
        if (target.empty()) target = "chat";
        const std::string path = experience::recordLesson(target, text);
        if (path.empty()) {
            ui::note("could not record the lesson");
            return;
        }
        ui::note("lesson recorded in " + path);
        refreshPracticeDigest();
        resetSystem();
    };

    auto recordChatStory = [&](const StoryRequest& request) {
        if (ui::testMode()) {
            ui::note("test mode: story memory is read-only");
            return;
        }
        const std::string text = trim(request.body);
        if (text.empty()) return;
        const bool ok = request.object.empty()
                            ? story::writeLife(text)
                            : story::writeObject(request.object, text);
        if (!ok) {
            ui::note("could not update story memory");
            return;
        }
        refreshStory();
        resetSystem();
        ui::note(request.object.empty()
                     ? "life story updated"
                     : "object story updated: " + request.object);
    };

    auto commitTracker = [&]() -> bool {
        std::string archiveNote;
        identity::compact(tracker, identity::kKeepTombstones, archiveNote);
        if (!identity::save(tracker)) {
            ui::note("could not save the identity tracker");
            return false;
        }
        trackerFull = identity::render(tracker);
        buildTrackerView();
        resetSystem();
        if (archiveNote.rfind("archived ", 0) == 0 ||
            archiveNote.rfind("could not ", 0) == 0)
            ui::note(archiveNote);
        return true;
    };

    auto snapshot = [&]() {
        std::lock_guard<std::mutex> lock(stateMutex);
        AutonomySnapshot s;
        s.history = history;
        s.summary = documentaryText;
        s.sca = cognition::load();
        return s;
    };

    auto initializeCognition = [&]() -> bool {
        if (!cognitionText.empty()) return true;
        if (ui::testMode()) return false;
        ui::note("no self-cognition file yet — composing one…");
        std::string saved;
        WaitTicker progress("self-cognition");
        progress.start();
        const bool ok = cognition::refresh(cfg, "", "", trackerFull, saved);
        progress.stop();
        if (ok) {
            cognitionText = saved;
            ui::note("self-cognition saved to " + cognition::filePath());
            return true;
        }
        ui::note("could not compose the self-cognition file — use /sca to try "
                 "again");
        return false;
    };

    // Leave test mode and return to ordinary chat behaviour. If there is no
    // .sca yet, this also performs the first initialization.
    auto leaveTestMode = [&] {
        ui::setTestMode(false);
        testWarned = false;
        // The startup binding was skipped in read-only mode; apply it now.
        if (!cfg.login_id.empty()) {
            std::string loginNote;
            if (!identity::bindLogin(tracker, cfg.login_id, loginNote)) {
                ui::note(loginNote);
            } else if (!loginNote.empty()) {
                if (commitTracker()) ui::note(loginNote);
            } else {
                buildTrackerView();
            }
        }
        ui::note("test mode off — memory is live again");
        initializeCognition();
        resetSystem();
    };

    // A short tail of the conversation, used when the self-cognition file is
    // refreshed outside the compaction pass.
    auto recentExcerpt = [&] {
        std::ostringstream out;
        for (size_t i = 1; i < history.size(); ++i) {
            const Message& m = history[i];
            if (m.role == "system") continue;
            const std::string who =
                m.role == "user"
                    ? (cfg.login_id.empty() ? std::string("User")
                                            : cfg.login_id)
                    : cfg.nick;
            out << who << ": " << m.content << "\n\n";
        }
        std::string text = out.str();
        constexpr size_t kMax = 6000;
        if (text.size() > kMax) {
            text.erase(0, text.size() - kMax);
            size_t nl = text.find('\n');
            if (nl != std::string::npos) text.erase(0, nl + 1);
            text = "…(earlier part omitted)…\n" + text;
        }
        return text;
    };

    auto refreshCognition = [&](const std::string& recent) {
        if (ui::testMode()) {
            ui::note("test mode: self-cognition is read-only");
            return false;
        }
        // The worker may have rewritten the file since this turn began. Use
        // the on-disk version as "previous" so a stale copy cannot clobber it.
        const std::string disk = cognition::load();
        if (!disk.empty()) cognitionText = disk;
        std::string saved;
        WaitTicker progress("self-cognition");
        progress.start();
        const bool ok = cognition::refresh(cfg, cognitionText, recent,
                                           trackerFull, saved);
        progress.stop();
        if (!ok) {
            ui::note("could not update the self-cognition file");
            return false;
        }
        cognitionText = saved;
        resetSystem();
        ui::note("self-cognition updated: " + cognition::filePath());
        return true;
    };

    // Once the context grows past the soft limit, turn the older turns into a
    // raw documentary item, organize the archive, and refresh self-cognition
    // in the same pass.
    auto compact = [&]() -> bool {
        size_t chars = 0;
        for (const Message& m : history) chars += m.content.size();
        if (ui::testMode()) {
            if (!testWarned && chars > CONTEXT_SOFT_LIMIT) {
                ui::note("test mode: context is long, but compaction and "
                         "memory writes are off");
                testWarned = true;
            }
            return false;
        }
        if (chars <= CONTEXT_SOFT_LIMIT || history.size() < 6) return false;

        constexpr size_t kKeep = 8; // recent messages kept verbatim
        if (history.size() <= 1 + kKeep) return false;
        size_t cut = history.size() - kKeep;
        while (cut < history.size() && history[cut].role != "user") ++cut;
        if (cut >= history.size() || cut <= 1) return false;
        std::vector<Message> older(history.begin() + 1, history.begin() + cut);
        if (older.size() < 4) return false;

        ui::note("context is getting long — recording documentary memory and "
                 "refreshing self-cognition…");
        const std::string periodEnd = documentary::now();
        std::string rawText;
        WaitTicker recordProgress("documentary record");
        recordProgress.start();
        const bool recorded = documentary::makeRaw(
            cfg, older, memorySegmentStart, periodEnd, rawText);
        recordProgress.stop();
        if (!recorded) {
            ui::note("could not record documentary memory — keeping the full "
                     "history for now");
            return false;
        }

        documentary::Item raw;
        raw.start = memorySegmentStart;
        raw.end = periodEnd;
        raw.text = rawText;
        const documentary::Memory before = documentaryMemory;
        documentary::addRaw(documentaryMemory, raw);
        if (!documentary::save(documentaryMemory)) {
            documentaryMemory = before;
            ui::note("could not save documentary memory — keeping the full "
                     "history for now");
            return false;
        }

        refreshCognition(rawText);

        if (documentary::needsOrganize(documentaryMemory)) {
            // This merge runs one or more blocking model calls. Without a
            // note, a slow merge looks exactly like a frozen session: the
            // prompt is gone, typed input queues in the pipe, and nothing is
            // printed until it finishes or curl's timeout fires.
            ui::note("organizing documentary memory — the chat waits until "
                     "this merge finishes…");
            documentaryMergeFailed = false;
            std::string mergeNote;
            WaitTicker mergeProgress("documentary merge");
            mergeProgress.start();
            const bool merged = documentary::organize(
                cfg, documentaryMemory, cognitionText, mergeNote);
            mergeProgress.stop();
            if (!merged && !mergeNote.empty()) {
                documentaryMergeFailed = true;
                ui::note(mergeNote);
            }
        }

        documentaryText = documentary::render(documentaryMemory, 8000);
        history.erase(history.begin() + 1, history.begin() + cut);
        resetSystem();
        memorySegmentStart = periodEnd;
        ui::note("conversation compacted — a raw documentary segment was "
                 "recorded");
        return true;
    };

    // Merge full documentary layers. This is also the "free time" trigger:
    // the chat calls it after a reply if a previous merge was left pending.
    auto organizeDocumentary = [&](bool manual) -> bool {
        if (ui::testMode()) {
            if (manual)
                ui::note("test mode: documentary memory is read-only");
            return false;
        }
        if (!documentary::needsOrganize(documentaryMemory)) {
            documentaryMergeFailed = false;
            if (manual) ui::note("documentary memory is already organized");
            return true;
        }
        ui::note("organizing documentary memory…");
        std::string mergeNote;
        WaitTicker mergeProgress("documentary merge");
        mergeProgress.start();
        const bool ok = documentary::organize(cfg, documentaryMemory,
                                              cognitionText, mergeNote);
        mergeProgress.stop();
        documentaryMergeFailed = !ok;
        documentaryText = documentary::render(documentaryMemory, 8000);
        resetSystem();
        if (!mergeNote.empty()) ui::note(mergeNote);
        return ok;
    };

    auto flushAutonomy = [&] {
        for (const std::string& message : autonomy.takeMessages())
            printWorkerNote(message);
    };

    // Permission prompts come from the worker thread, but the main thread
    // owns the terminal, so it asks here and then releases the worker.
    auto printable = [](const std::string& text) {
        std::string shown = text;
        for (char& c : shown) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7f) c = '?';
        }
        return shown;
    };

    auto handleLocalDirRequest = [&] {
        local_dir_access::Pending request = local_dir_access::pending();
        if (!request.active) return;

        const std::string shown = printable(request.path);

        ui::note("the AI requests read access to a local directory:");
        std::printf("  %s%s%s\n", ui::bold(), shown.c_str(), ui::off());
        std::printf("%sAllow read access for this session? [y/N] %s",
                    ui::dim(), ui::off());
        std::fflush(stdout);

        std::string answer;
        const bool gotAnswer = readLine(answer);
        std::string normalized = trim(answer);
        for (char& c : normalized)
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        const bool allow =
            gotAnswer && (normalized == "y" || normalized == "yes");

        if (!local_dir_access::decide(allow)) {
            ui::note("the directory-access request expired");
            return;
        }
        if (allow)
            sessionNote("granted read access for this session: " + shown);
        else
            sessionNote("denied read access: " + shown);
    };

    auto handleSelfMadeToolRequest = [&] {
        self_made_tools::Pending request = self_made_tools::pending();
        if (!request.active) return;

        const std::string name = printable(request.name);
        const std::string op = printable(request.op);
        std::string explanation = printable(request.explanation);
        explanation = text_utf8::head(explanation, 1200);

        ui::note("the AI requests permission to change a self-made tool:");
        std::printf("  tool: %s%s%s\n", ui::bold(), name.c_str(), ui::off());
        std::printf("  action: %s%s%s\n", ui::bold(), op.c_str(), ui::off());
        std::printf("  explanation: %s\n", explanation.c_str());
        std::printf("%sAllow this change? [y/N] %s", ui::dim(), ui::off());
        std::fflush(stdout);

        std::string answer;
        const bool gotAnswer = readLine(answer);
        std::string normalized = trim(answer);
        for (char& c : normalized)
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        const bool allow =
            gotAnswer && (normalized == "y" || normalized == "yes");

        if (!self_made_tools::decide(allow)) {
            ui::note("the self-made tool request expired");
            return;
        }
        if (allow)
            sessionNote("approved " + op + " of self-made tool: " + name);
        else
            sessionNote("denied " + op + " of self-made tool: " + name);
    };

    auto handleSandboxMirrorRequest = [&] {
        sandbox_mirror::Pending request = sandbox_mirror::pending();
        if (!request.active) return;

        const std::string kind = request.directory ? "directory" : "file";
        const std::string origin = printable(request.origin);
        const std::string mirror = printable(request.mirror);
        const std::string replaceDestination =
            printable(request.replaceDestination);
        const std::string newDestination = printable(request.newDestination);
        std::string explanation = printable(request.explanation);
        explanation = text_utf8::head(explanation, 1200);

        ui::note("the AI requests permission to apply a mirrored " + kind +
                 " back:");
        std::printf("  origin: %s%s%s\n", ui::bold(), origin.c_str(),
                    ui::off());
        std::printf("  mirror: %s\n", mirror.c_str());
        std::printf("  explanation: %s\n", explanation.c_str());
        std::printf("  %s[r]%seplace: %s\n", ui::bold(), ui::off(),
                    replaceDestination.c_str());
        std::printf("  %s[n]%sew:     %s\n", ui::bold(), ui::off(),
                    newDestination.c_str());
        std::printf("%sApply [r]eplace / [n]ew / [N] DO NOT APPLY: %s",
                    ui::dim(), ui::off());
        std::fflush(stdout);

        std::string answer;
        const bool gotAnswer = readLine(answer);
        const std::string trimmed = trim(answer);
        sandbox_mirror::ApplyChoice choice = sandbox_mirror::ApplyChoice::Deny;
        if (gotAnswer && trimmed != "N") {
            std::string normalized = trimmed;
            for (char& c : normalized)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            if (normalized == "r" || normalized == "replace")
                choice = sandbox_mirror::ApplyChoice::Replace;
            else if (normalized == "n" || normalized == "new")
                choice = sandbox_mirror::ApplyChoice::New;
        }

        if (!sandbox_mirror::decide(choice)) {
            ui::note("the sandbox mirror request expired");
            return;
        }
        if (choice == sandbox_mirror::ApplyChoice::Replace)
            sessionNote("replaced mirror origin: " + replaceDestination);
        else if (choice == sandbox_mirror::ApplyChoice::New)
            sessionNote("applied mirror as new path: " + newDestination);
        else
            sessionNote("DO NOT APPLY: mirror not applied: " + origin);
    };

    int turnTimeout = cfg.timeout; // the countdown for the next turn
    bool resumedAutonomy = false;  // <auto/> resumed a paused loop this turn

    // A session can end in several ways. Whatever the outcome, the model is
    // given one chat turn to tell the user what happened before the next
    // countdown starts; the mechanical summary is only the fallback.
    auto applyAutonomyResult = [&] {
        if (!autonomy.finished()) return;
        AutonomyResult r = autonomy.takeResult();
        if (!r.target.empty()) lastExperienceTarget = r.target;
        refreshStory();
        flushAutonomy();

        std::printf("\n");
        if (!r.ok) {
            printEndHeader("ended", ui::red(), r.steps, r.energyLeft);
            printEndField(
                "reason",
                r.reason.empty() ? std::string("unknown error") : r.reason,
                ui::red());
            std::printf("\n");
        } else if (r.tired) {
            printEndHeader("stopped tired", ui::yellow(), r.steps,
                           r.energyLeft);
        } else if (r.gaveUp) {
            printEndHeader("stopped", ui::yellow(), r.steps, r.energyLeft);
            if (!r.reason.empty())
                printEndField("reason", r.reason, ui::yellow());
        } else {
            printEndHeader("finished", ui::green(), r.steps, r.energyLeft);
        }
        if (r.ok) {
            printEndBlock(
                "target",
                r.target.empty() ? std::string("(none)") : r.target);
            printEndField(
                "outcome",
                r.outcome.empty() ? std::string("(none)") : r.outcome,
                r.gaveUp || r.tired ? ui::yellow() : ui::green());
            printEndBlock(
                "reflection",
                r.reflection.empty() ? std::string("(none)") : r.reflection);
            std::printf("\n");
        }

        if (r.cognitionUpdated && !r.newSca.empty()) {
            std::lock_guard<std::mutex> lock(stateMutex);
            cognitionText = r.newSca;
        }

        if (!r.ok) {
            // The API is already failing; do not spend another call on a
            // report. Keep the mechanical summary in the transcript.
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!r.summary.empty())
                history.push_back({"assistant", r.summary});
            if (!history.empty()) history[0].content = systemText();
            return;
        }

        // Switch back to chat with an explicit information hand-off. The
        // note describes the finished session, and the model turns it into a
        // short message to the user. This runs before the next readTurn(), so
        // the report always arrives before a new turn begins.
        std::ostringstream note;
        note << "[AUTONOMY ENDED] Your background autonomous session has "
                "ended. ";
        if (r.tired)
            note << "It ran out of energy";
        else if (r.gaveUp)
            note << "You gave up";
        else
            note << "You completed the target";
        if (!r.reason.empty()) note << " (" << r.reason << ")";
        note << ". Steps taken: " << r.steps << "; energy left: "
             << amountText(r.energyLeft) << "/" << amountText(1000.0) << ".";
        if (!r.summary.empty()) note << " Session summary: " << r.summary;
        note << " The user is back in chat. Before the next turn starts, tell "
                "the user in your own voice and in a few sentences what you "
                "did, how it went, why you stopped or gave up if you did, and "
                "what you plan next. Be honest and concise. Do not start "
                "another autonomous loop or mention internal mechanics, tags, "
                "or this note.";
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            history.push_back({"user", note.str()});
            if (!history.empty()) history[0].content = systemText();
        }

        std::string report;
        bool reportWait = false;
        bool reportCognition = false;
        bool reportAutonomy = false;
        int reportTimer = 0;
        std::vector<TrackRequest> reportTracks;
        std::vector<std::string> reportLessons;
        std::vector<StoryRequest> reportStories;
        std::string prefix = std::string(ui::cyan()) + ui::bold() + cfg.nick +
                             " > " + ui::off();
        if (streamChat(cfg, history, prefix, report, reportWait, reportCognition,
                       reportAutonomy, reportTimer, false, reportTracks,
                       reportLessons, reportStories)) {
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                if (!report.empty())
                    history.push_back({"assistant", report});
                else if (!r.summary.empty())
                    history.push_back({"assistant", r.summary});
            }
            for (const TrackRequest& request : reportTracks) {
                if (ui::testMode()) break;
                std::string note;
                if (identity::addLine(tracker, request.id, request.kind,
                                      request.text, note))
                    commitTracker();
            }
            for (const std::string& lesson : reportLessons)
                recordChatLesson(lesson);
            for (const StoryRequest& request : reportStories)
                recordChatStory(request);
            // The report is an ordinary chat reply, so a timer it picked
            // applies to the upcoming turn. A <wait/> is ignored here: the
            // user did not ask for an untimed pause.
            if (cfg.timeout > 0 && reportTimer > 0) turnTimeout = reportTimer;
            std::printf("\n\n");
        } else {
            // The hand-off message failed; fall back to the summary so the
            // user still learns what happened.
            std::lock_guard<std::mutex> lock(stateMutex);
            history.pop_back(); // the [AUTONOMY ENDED] note
            if (!r.summary.empty())
                history.push_back({"assistant", r.summary});
            if (!history.empty()) history[0].content = systemText();
            sessionNote("could not compose the autonomous result message");
        }
    };

    auto startAutonomy = [&]() -> bool {
        if (autonomy.finished()) applyAutonomyResult();
        if (ui::testMode()) {
            sessionNote("test mode: autonomous work is off");
            return false;
        }
        if (autonomy.paused()) {
            autonomy.resume();
            resumedAutonomy = true;
            sessionNote("resuming paused autonomous work");
            return true;
        }
        if (autonomy.busy()) {
            sessionNote("an autonomous loop is already running — /auto status");
            return false;
        }
        if (cognitionText.empty()) {
            sessionNote("no self-cognition file: run /sca first");
            return false;
        }
        if (!autonomy.start(cfg, ui::testMode(), snapshot)) {
            sessionNote("could not start the autonomous loop");
            return false;
        }
        // Do not touch the history here: the worker may already be taking a
        // snapshot. The system prompt is refreshed before the next chat turn.
        sessionNote("autonomous loop started in the background — type to "
                    "interrupt, /auto stop to end it");
        return true;
    };

    // After a command that is not itself chat, let a paused worker continue.
    auto resumeAfterCommand = [&] {
        if (autonomy.paused()) {
            autonomy.resume();
            sessionNote("resuming autonomous work");
        }
    };

    ui::banner(cfg);
    if (tagManuals.empty())
        ui::note("tag manuals not found in ai-tools — using the built-in tag "
                 "instructions");

    // First initialization: there is no .sca yet, so have the model write one
    // before the first turn. The questions .md is created lazily by refresh().
    // Test mode is read-only, so it skips this.
    if (!initializeCognition() && ui::testMode())
        ui::note("test mode: no .sca will be written; memory is read-only");

    history.push_back({"system", systemText()});

    bool waitMode = false;

    for (;;) {
        if (autonomy.finished()) applyAutonomyResult();

        // Pick up a manual edit to either memory file before this turn's
        // prompt is built. Ignore a missing .sca so a transient read failure
        // cannot erase the identity the session already has.
        {
            const std::string disk = cognition::load();
            bool changed = false;
            if (!disk.empty() && disk != cognitionText) {
                cognitionText = disk;
                changed = true;
            }
            identity::Tracker fresh = identity::load();
            const std::string freshFull = identity::render(fresh);
            if (freshFull != trackerFull) {
                tracker = std::move(fresh);
                trackerFull = freshFull;
            }
            const std::string beforeView = trackerText;
            buildTrackerView();
            if (beforeView != trackerText) changed = true;
            const std::string beforeStory = storyText;
            refreshStory();
            if (beforeStory != storyText) changed = true;
            if (changed) resetSystem();
        }

        std::string line;
        bool userAskedNoTimer = false;
        bool userGaveExactTime = false;
        Turn t;
        if (autonomy.running() || autonomy.finished()) {
            // No countdown while the background loop runs; the tick flushes
            // its status lines and tells us when it has finished. Watching
            // `finished` too closes the race where the worker finishes
            // between the check above and entering readTurn().
            t = readTurn(
                0, line,
                [&] {
                    flushAutonomy();
                    return autonomy.running() &&
                           !local_dir_access::hasPending() &&
                           !self_made_tools::hasPending() &&
                           !sandbox_mirror::hasPending();
                },
                [&] { return autonomy.liveStatus(); });
        } else {
            // Close the tiny race where the worker finishes just after the
            // check at the top of the loop.
            if (autonomy.finished()) {
                applyAutonomyResult();
                continue;
            }
            t = readTurn(waitMode ? 0 : turnTimeout, line);
        }

        if (t == Turn::Eof) {
            local_dir_access::cancel("the chat is exiting");
            self_made_tools::cancel("the chat is exiting");
            sandbox_mirror::cancel("the chat is exiting");
            autonomy.stop();
            std::printf("\n%sbye%s\n", ui::dim(), ui::off());
            return;
        }

        if (t == Turn::Background) {
            flushAutonomy();
            handleLocalDirRequest();
            handleSelfMadeToolRequest();
            handleSandboxMirrorRequest();
            continue;
        }

        if (t == Turn::Timeout) {
            if (autonomy.paused()) {
                sessionNote("time's up — resuming autonomous work");
                autonomy.resume();
                waitMode = false;
                turnTimeout = cfg.timeout;
                continue;
            }
            std::printf("%s*time's up — asking the model what it wants to do%s\n",
                        ui::yellow(), ui::off());
            std::ostringstream note;
            note << "[TIMEOUT] The user stayed silent for " << turnTimeout
                 << " seconds. Decide what happens next: either continue the "
                    "conversation by yourself, or — if you really need the "
                    "user — ask a short question and end your reply with "
                 << WAIT_TAG << ".";
            history.push_back({"user", note.str()});
            viewFocus = note.str();
            buildTrackerView();
        } else {
            line = trim(line);
            if (line.empty()) continue;

            // The potion is a user-side effect, not a slash command. Refill
            // before an autonomous loop is paused, so the worker sees it as
            // soon as the message is sent. The marker stays in `line` for now
            // so a message that starts with it is never mistaken for a slash
            // command; it is stripped below, after the pause.
            const std::string typedLine = line;
            bool potionUsed = false;
            if (typedLine[0] != '/') {
                std::string probe = typedLine;
                potionUsed = takeEnergyPotionMarker(probe);
                if (potionUsed) autonomy.refillEnergy();
            }

            // Quit must reach stop() directly: stop() kills an in-flight API
            // request, while pause() would wait for the worker to reach a
            // checkpoint it may never get to. Test-mode /exit is not a quit.
            if (line == "/exit" && ui::testMode()) {
                leaveTestMode();
                continue;
            }
            if (line == "/quit" || line == "/exit" || line == "/q") {
                local_dir_access::cancel("the chat is exiting");
                self_made_tools::cancel("the chat is exiting");
                sandbox_mirror::cancel("the chat is exiting");
                autonomy.stop();
                std::printf("%sbye%s\n", ui::dim(), ui::off());
                return;
            }

            // The worker may have finished while the user was typing. Apply
            // its result before responding to the line.
            if (autonomy.finished()) applyAutonomyResult();

            bool interrupted = autonomy.running();
            if (interrupted) {
                if (local_dir_access::hasPending())
                    local_dir_access::cancel(
                        "the user interrupted the permission request");
                if (self_made_tools::hasPending())
                    self_made_tools::cancel(
                        "the user interrupted the permission request");
                if (sandbox_mirror::hasPending())
                    sandbox_mirror::cancel(
                        "the user interrupted the permission request");
                sessionNote("pausing autonomous work for your message…");
                autonomy.pause();
                flushAutonomy();
                // The worker may have finished before it could acknowledge
                // the pause.
                if (autonomy.finished()) applyAutonomyResult();
            }

            if (line == "/help") {
                ui::help();
                resumeAfterCommand();
                continue;
            }
            if (line == "/new") {
                viewFocus.clear();
                buildTrackerView();
                history.assign(1, {"system", systemText()});
                memorySegmentStart = documentary::now();
                waitMode = false;
                turnTimeout = cfg.timeout;
                std::printf("%sfresh conversation (long-term memory is kept)%s\n\n",
                            ui::dim(), ui::off());
                resumeAfterCommand();
                continue;
            }
            if (line == "/login" || startsWith(line, "/login ")) {
                const std::string arg =
                    line == "/login"
                        ? std::string()
                        : trim(line.substr(strlen("/login ")));
                if (arg.empty()) {
                    std::string info = "login-id: " + cfg.login_id;
                    if (const identity::Object* me =
                            identity::objectForLogin(tracker, cfg.login_id))
                        info += " — [" + me->category + "] " + me->name +
                                " (" + me->id + ")";
                    sessionNote(info);
                    resumeAfterCommand();
                    continue;
                }

                const std::string clean = normalizeLoginId(arg);
                if (clean.empty()) {
                    ui::note("invalid login-id (use letters, digits, '.', "
                             "'_', '-' or '@', at most 64 bytes)");
                    resumeAfterCommand();
                    continue;
                }
                if (clean == cfg.login_id) {
                    sessionNote("already logged in as " + clean);
                    resumeAfterCommand();
                    continue;
                }

                if (autonomy.busy()) {
                    autonomy.stop();
                    sessionNote("stopped autonomous work before switching "
                                "login-id");
                }

                cfg.login_id = clean;
                ui::setLoginId(clean);
                if (!ui::testMode()) {
                    if (!saveConfig(cfg))
                        ui::note("could not save the login-id to the config; "
                                 "it is used for this session only");
                    std::string loginNote;
                    if (!identity::bindLogin(tracker, clean, loginNote)) {
                        ui::note(loginNote);
                    } else if (!loginNote.empty() && commitTracker()) {
                        ui::note(loginNote);
                    }
                } else {
                    buildTrackerView();
                }

                // A different person must not inherit the previous person's
                // working conversation. Long-term memory is shared.
                manualFocus.clear();
                viewFocus.clear();
                buildTrackerView();
                history.assign(1, {"system", systemText()});
                memorySegmentStart = documentary::now();
                waitMode = false;
                turnTimeout = cfg.timeout;
                sessionNote("switched to login-id " + clean +
                            " — fresh conversation");
                resumeAfterCommand();
                continue;
            }
            if (line == "/nick" || startsWith(line, "/nick ")) {
                std::string rest = trim(line.c_str() + (line == "/nick" ? 5 : 6));
                if (rest.empty()) {
                    std::printf("%sthe AI is called %s%s%s\n\n", ui::dim(),
                                ui::bold(), cfg.nick.c_str(), ui::off());
                    resumeAfterCommand();
                    continue;
                }
                cfg.nick = rest;
                resetSystem();
                std::printf("%sthe AI is now called %s%s%s\n\n", ui::dim(),
                            ui::bold(), cfg.nick.c_str(), ui::off());
                resumeAfterCommand();
                continue;
            }
            if (startsWith(line, "/timeout")) {
                int v = atoi(line.c_str() + strlen("/timeout"));
                if (v < 0) v = 0;
                cfg.timeout = v;
                turnTimeout = v;
                waitMode = false;
                resetSystem();
                std::printf("%syou now get %ds per turn%s\n\n", ui::dim(), v,
                            ui::off());
                resumeAfterCommand();
                continue;
            }
            if (line == "/memory" || startsWith(line, "/memory ")) {
                std::string sub = line == "/memory"
                                      ? std::string()
                                      : trim(line.substr(strlen("/memory ")));
                if (sub.empty() || sub == "show") {
                    std::printf("%sdocumentary memory: %s%s\n\n", ui::dim(),
                                documentary::path().c_str(), ui::off());
                    const std::string text =
                        documentary::render(documentaryMemory);
                    if (text.empty())
                        ui::note("documentary memory is empty");
                    else
                        std::printf("%s%s%s\n", ui::gray(), text.c_str(),
                                    ui::off());
                    resumeAfterCommand();
                    continue;
                }
                if (sub == "path") {
                    std::printf("%s%s%s\n\n", ui::dim(),
                                documentary::path().c_str(), ui::off());
                    resumeAfterCommand();
                    continue;
                }
                if (sub == "organize") {
                    documentaryMergeFailed = false;
                    organizeDocumentary(true);
                    resumeAfterCommand();
                    continue;
                }
                sessionNote("usage: /memory, /memory show, /memory organize");
                resumeAfterCommand();
                continue;
            }
            if (line == "/remember" || startsWith(line, "/remember ")) {
                ui::note("use /track [Category] Name: identity; ... "
                         "(or /track to show the tracker)");
                resumeAfterCommand();
                continue;
            }
            if (line == "/focus" || startsWith(line, "/focus ")) {
                const std::string arg =
                    line == "/focus"
                        ? std::string()
                        : trim(line.substr(strlen("/focus ")));
                if (arg.empty()) {
                    if (manualFocus.empty())
                        ui::note("focus: automatic (the current login-id)");
                    else
                        std::printf("%sfocus: %s%s\n\n", ui::dim(),
                                    manualFocus.front().c_str(), ui::off());
                } else if (arg == "clear") {
                    manualFocus.clear();
                    buildTrackerView();
                    resetSystem();
                    ui::note("focus cleared — using the current login-id");
                } else if (arg.size() > 200) {
                    ui::note("focus is too long (max 200 characters)");
                } else {
                    manualFocus.assign(1, arg);
                    buildTrackerView();
                    resetSystem();
                    ui::note("focus set to: " + arg);
                }
                resumeAfterCommand();
                continue;
            }
            if (line == "/track" || line == "/tracker" ||
                startsWith(line, "/track ") ||
                startsWith(line, "/tracker ")) {
                const std::string arg =
                    line == "/track" || line == "/tracker"
                        ? std::string()
                        : trim(line.substr(line.find(' ') + 1));
                if (arg == "path") {
                    std::printf("%s%s%s\n\n", ui::dim(),
                                identity::path().c_str(), ui::off());
                    resumeAfterCommand();
                    continue;
                }
                if (arg == "history") {
                    std::printf("%s%s%s\n\n", ui::dim(),
                                identity::historyPath().c_str(), ui::off());
                    resumeAfterCommand();
                    continue;
                }
                if (arg == "compact") {
                    if (ui::testMode()) {
                        ui::note("test mode: the identity tracker is read-only");
                        resumeAfterCommand();
                        continue;
                    }
                    std::string note;
                    if (!identity::compact(tracker, 0, note)) {
                        ui::note(note);
                    } else if (commitTracker()) {
                        ui::note(note);
                    }
                    resumeAfterCommand();
                    continue;
                }
                if (arg.empty()) {
                    const std::string full = identity::render(tracker);
                    if (full.empty())
                        ui::note("the identity tracker is empty");
                    else
                        std::printf("%s%s%s\n", ui::gray(), full.c_str(),
                                    ui::off());
                    resumeAfterCommand();
                    continue;
                }
                if (ui::testMode()) {
                    ui::note("test mode: the identity tracker is read-only");
                    resumeAfterCommand();
                    continue;
                }
                std::string id;
                std::string note;
                if (!identity::track(tracker, arg, id, note)) {
                    ui::note(note);
                    resumeAfterCommand();
                    continue;
                }
                if (commitTracker()) ui::note(note);
                resumeAfterCommand();
                continue;
            }
            if (line == "/merge" || startsWith(line, "/merge ")) {
                std::string rest = trim(line.substr(strlen("/merge")));
                std::string reason;
                const size_t bar = rest.find('|');
                if (bar != std::string::npos) {
                    reason = trim(rest.substr(bar + 1));
                    rest = trim(rest.substr(0, bar));
                }
                size_t sep = rest.find(" into ");
                size_t sepLen = 6;
                if (sep == std::string::npos) {
                    sep = rest.find(" -> ");
                    sepLen = 4;
                }
                if (sep == std::string::npos) {
                    sessionNote("usage: /merge FROM into TO [| reason]");
                    resumeAfterCommand();
                    continue;
                }
                const std::string from = trim(rest.substr(0, sep));
                const std::string into = trim(rest.substr(sep + sepLen));
                if (ui::testMode()) {
                    ui::note("test mode: the identity tracker is read-only");
                    resumeAfterCommand();
                    continue;
                }
                std::string note;
                if (!identity::merge(tracker, from, into, reason, note)) {
                    ui::note(note);
                    resumeAfterCommand();
                    continue;
                }
                if (commitTracker()) ui::note(note);
                resumeAfterCommand();
                continue;
            }
            if (line == "/forget" || startsWith(line, "/forget ")) {
                const std::string arg =
                    line == "/forget"
                        ? std::string()
                        : trim(line.substr(strlen("/forget ")));
                if (arg.empty()) {
                    sessionNote("usage: /forget ID-OR-NAME");
                    resumeAfterCommand();
                    continue;
                }
                if (ui::testMode()) {
                    ui::note("test mode: the identity tracker is read-only");
                    resumeAfterCommand();
                    continue;
                }
                std::string note;
                if (!identity::forget(tracker, arg, note)) {
                    ui::note(note);
                    resumeAfterCommand();
                    continue;
                }
                if (commitTracker()) ui::note(note);
                resumeAfterCommand();
                continue;
            }
            if (line == "/sca") {
                refreshCognition(recentExcerpt());
                resumeAfterCommand();
                continue;
            }
            if (line == "/auto" || startsWith(line, "/auto ")) {
                std::string sub = line == "/auto"
                                      ? std::string()
                                      : trim(line.substr(strlen("/auto ")));
                if (sub == "stop") {
                    if (autonomy.busy())
                        sessionNote("stopping autonomous work…");
                    autonomy.stop();
                    continue;
                }
                if (sub == "status") {
                    printAutonomyStatus(autonomy.status());
                    std::printf("\n");
                    resumeAfterCommand();
                    continue;
                }
                if (!sub.empty()) {
                    sessionNote("usage: /auto, /auto stop, /auto status");
                    resumeAfterCommand();
                    continue;
                }
                if (autonomy.paused()) {
                    autonomy.resume();
                    sessionNote("resuming autonomous work");
                } else if (autonomy.running()) {
                    sessionNote("the autonomous loop is already running");
                } else {
                    startAutonomy();
                }
                continue;
            }
            if (line == "/test") {
                if (ui::testMode()) {
                    leaveTestMode();
                } else {
                    if (autonomy.busy()) {
                        sessionNote("stopping autonomous work before entering "
                                    "test mode");
                        autonomy.stop();
                    }
                    ui::setTestMode(true);
                    testWarned = false;
                    ui::note("test mode on — .sca, the identity tracker and "
                             "documentary memory are read-only");
                    resetSystem();
                }
                continue;
            }
            if (!line.empty() && line[0] == '/') {
                std::printf("%sunknown command, try /help%s\n", ui::dim(),
                            ui::off());
                resumeAfterCommand();
                continue;
            }
            userAskedNoTimer = userRequestsUntimedWait(typedLine);
            userGaveExactTime = userRequestsExactTime(typedLine);
            ui::echoUser(typedLine);
            if (potionUsed) {
                takeEnergyPotionMarker(line);
                // The worker may have spent a step while it was acknowledging
                // the pause. Refill again so the chat turn really starts full.
                autonomy.refillEnergy();
                const std::string energy =
                    amountText(autonomy.energyLeft());
                const std::string full = amountText(1000.0);
                sessionNote("energy potion used — energy is full (" + energy +
                            "/" + full + ")");
                const std::string note =
                    "[The user used an energy potion. Your energy is now full "
                    "(" +
                    energy + "/" + full + ").]";
                line = line.empty() ? note : line + "\n\n" + note;
            }
            history.push_back({"user", line});
            viewFocus = typedLine;
            buildTrackerView();
        }

        // Refresh the clock in the system prompt so the model can turn a
        // wall-clock request ("wait until 15:30") into a number of seconds.
        resetSystem();

        const bool pausedLoop = autonomy.paused();
        std::string reply;
        bool wantsWait = false;
        bool wantsCognition = false;
        bool wantsAutonomy = false;
        int aiTimerSec = 0;
        std::vector<TrackRequest> trackRequests;
        std::vector<std::string> lessonRequests;
        std::vector<StoryRequest> storyRequests;
        std::string prefix = std::string(ui::cyan()) + ui::bold() + cfg.nick +
                             " > " + ui::off();
        if (!streamChat(cfg, history, prefix, reply, wantsWait, wantsCognition,
                        wantsAutonomy, aiTimerSec, userGaveExactTime,
                        trackRequests, lessonRequests, storyRequests)) {
            history.pop_back(); // drop the message that failed
            waitMode = false;
            turnTimeout = cfg.timeout;
            std::printf("\n");
            // A failed chat turn should not strand a paused loop; let it
            // continue on the next iteration.
            if (pausedLoop) autonomy.resume();
            continue;
        }
        if (!reply.empty()) history.push_back({"assistant", reply});

        // The model may ask the app to record a confirmed identity. The app
        // validates it, writes it, and rebuilds the prompt before the next
        // turn; the model never touches the file.
        for (const TrackRequest& request : trackRequests) {
            if (ui::testMode()) {
                ui::note("test mode: the identity tracker is read-only");
                continue;
            }
            std::string note;
            if (!identity::addLine(tracker, request.id, request.kind,
                                   request.text, note)) {
                ui::note(note);
                continue;
            }
            if (commitTracker()) ui::note(note);
        }

        // The model may append a practice lesson without entering the
        // autonomous loop. The app owns the target and the file write.
        for (const std::string& lesson : lessonRequests)
            recordChatLesson(lesson);
        for (const StoryRequest& request : storyRequests)
            recordChatStory(request);

        // The model may skip the countdown before it starts, but only when
        // the user explicitly asked for it. A reply to the [TIMEOUT] note may
        // always choose the no-timer wait, since the countdown already ran.
        waitMode = wantsWait && (t == Turn::Timeout || userAskedNoTimer);
        // While a loop is paused, keep a countdown so silence resumes the
        // loop. Only an explicit untimed-wait request may disable it.
        if (pausedLoop && !userAskedNoTimer) waitMode = false;

        // The model picks the next countdown itself (clamped to 30-180s on
        // its own, wider when the user gave an exact time); the configured
        // value is the fallback.
        turnTimeout = (cfg.timeout > 0 && aiTimerSec > 0) ? aiTimerSec
                                                          : cfg.timeout;
        if (pausedLoop && turnTimeout <= 0) turnTimeout = 30;
        if (reply.empty() && !wantsWait && !wantsCognition && !wantsAutonomy &&
            aiTimerSec == 0 && trackRequests.empty())
            std::printf("%s(the model returned nothing)%s\n", ui::dim(),
                        ui::off());
        if (waitMode) {
            std::printf("\n%s* the model is waiting for you — no countdown%s\n\n",
                        ui::dim(), ui::off());
        } else {
            // End the reply and leave a blank line before the next pair.
            std::printf("\n\n");
        }
        // The model said its self-cognition file no longer fits. Rewrite it
        // from the old version plus the recent conversation. If the context
        // was compacted, that pass already refreshed the file.
        bool compacted = compact();
        if (wantsCognition && !compacted) {
            ui::note("the model asked to rewrite its self-cognition…");
            refreshCognition(recentExcerpt());
        }
        // Free-time trigger: a merge that failed or was left pending is
        // retried after a reply, when the next turn is not waiting on it.
        if (!compacted && !documentaryMergeFailed &&
            documentary::needsOrganize(documentaryMemory))
            organizeDocumentary(false);

        if (wantsAutonomy) {
            if (ui::testMode())
                sessionNote("test mode: autonomous work is off");
            else {
                resumedAutonomy = false;
                // If <auto/> resumed a paused loop, go straight back to the
                // worker instead of waiting for another chat turn.
                if (startAutonomy() && resumedAutonomy) continue;
            }
        }

        if (pausedLoop) {
            if (userRequestsContinue(line)) {
                autonomy.resume();
                sessionNote("resuming autonomous work");
            }
            // Otherwise remain paused; the countdown above will resume the
            // loop if the user stays silent.
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    ui::setColorEnabled(isatty(STDOUT_FILENO) != 0);

    Options opt;
    switch (parseArgs(argc, argv, opt)) {
        case Action::Help:     ui::usage(); return 0;
        case Action::SelfTest: return selfTest();
        case Action::MigrateExperience: {
            if (!ui::setupAiChatHome()) return 1;
            std::string report;
            const bool ok = experience::migrate(report, opt.migrateDryRun);
            std::printf("%s", report.c_str());
            return ok ? 0 : 1;
        }
        case Action::Error:    return 2;
        case Action::Run:      break;
    }

    if (!ui::setupAiChatHome()) return 1;

    Config cfg = loadConfig();
    applyOptions(cfg, opt);
    ui::setTestMode(opt.test);

    if (cfg.api_key.empty()) {
        if (!isatty(STDIN_FILENO)) {
            std::fprintf(stderr,
                         "ai-chat: no API key. Set OPENAI_API_KEY or pass "
                         "--api-key.\n");
            return 1;
        }
        if (!ui::setupWizard(cfg)) return 1;
    }

    // A login-id identifies the person at this terminal. It is asked at every
    // startup unless it was supplied explicitly. It is not the identity
    // tracker and not authentication.
    const char* envLogin = getenv("AI_CHAT_LOGIN_ID");
    const std::string envLoginId =
        envLogin ? normalizeLoginId(envLogin) : std::string();
    if (envLogin && *envLogin && envLoginId.empty())
        std::fprintf(stderr,
                     "ai-chat: ignoring invalid AI_CHAT_LOGIN_ID %s\n",
                     envLogin);
    const bool loginFixed = opt.hasLoginId || !envLoginId.empty();
    if (!ui::setupLogin(cfg, isatty(STDIN_FILENO) && !loginFixed)) return 1;
    ui::setLoginId(cfg.login_id);

    // The self-cognition files live in the config directory. The questions
    // markdown is created lazily when a new .sca is first written; an
    // existing questions file is never overwritten, so you can edit it.
    const std::string tagManuals = tag_manuals::all();
    self_made_tools::loadGrants();
    chatLoop(cfg, tagManuals);
    return 0;
}
