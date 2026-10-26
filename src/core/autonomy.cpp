#include "aichat/core/autonomy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "aichat/core/action.h"
#include "aichat/core/chat.h"
#include "aichat/core/cognition.h"
#include "aichat/core/experience.h"
#include "aichat/core/story.h"
#include "aichat/core/facts/store.h"
#include "aichat/core/guards.h"
#include "aichat/core/identity.h"
#include "aichat/core/motivation.h"
#include "aichat/core/util.h"
#include "ai-tools/text-utf8/text-utf8.h"
#include "ai-tools/local-dir-access/local-dir-access.h"
#include "ai-tools/sandbox-mirror/sandbox-mirror.h"
#include "ai-tools/self-made-tool/self-made-tool.h"

namespace {

constexpr double kEnergyFull = 1000.0;
constexpr double kEnergyPerMinute = 5.0;
constexpr int kMaxCycles = 200;
constexpr size_t kChatContextChars = 8000;
constexpr size_t kJournalChars = 12000;
constexpr size_t kRecentJournalChars = 6000;
constexpr size_t kActionResultChars = 6000;

// The live prompt rotates one frame every 100 ms. After this much silence
// (no worker update and no bytes from curl), it freezes instead of pretending
// the worker is still making progress.
constexpr long long kStallAfterMs = 30000;
constexpr long long kSpinnerFrameMs = 100;
const char* const kSpinnerFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                     "⠴", "⠦", "⠧", "⠇", "⠏"};
constexpr long long kSpinnerFrameCount =
    static_cast<long long>(sizeof(kSpinnerFrames) /
                           sizeof(kSpinnerFrames[0]));

long long monotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    while (s.find("  ") != std::string::npos)
        s.replace(s.find("  "), 2, " ");
    return trim(s);
}

std::string tailOf(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return "…(earlier part omitted)…\n" + text_utf8::tailLines(s, maxChars);
}

// Cuts `s` to `maxBytes` without splitting a UTF-8 code point. All prompt
// excerpts go through here; a bare substr() on model or tool text is a bug.
std::string cutText(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    return text_utf8::head(s, maxBytes) + "…";
}

std::string extractTag(const std::string& text, const char* name) {
    const std::string open = std::string("<") + name + ">";
    const std::string close = std::string("</") + name + ">";
    size_t p = text.find(open);
    if (p == std::string::npos) return "";
    p += open.size();
    size_t e = text.find(close, p);
    if (e == std::string::npos) e = text.size();
    return trim(text.substr(p, e - p));
}

std::string firstLine(const std::string& s) {
    std::string t = trim(s);
    size_t nl = t.find('\n');
    return nl == std::string::npos ? t : t.substr(0, nl);
}

// The analysis model sometimes prefixes the target with provenance commentary
// ("Quoted from ... I did not open .sca"). Keep the instruction, not the
// hedge. Curly quotes are accepted because the model may emit either form.
std::string cleanTarget(const std::string& raw) {
    std::string t = trim(raw);
    const bool hedged =
        startsWith(t, "Quoted from") || startsWith(t, "From the self-cognition") ||
        startsWith(t, "The target is") || startsWith(t, "Target:");
    if (!hedged) return t;

    const char* pairs[][2] = {{"\"", "\""}, {"\xE2\x80\x9C", "\xE2\x80\x9D"}};
    for (const auto& pair : pairs) {
        const size_t a = t.find(pair[0]);
        if (a == std::string::npos) continue;
        const size_t b = t.find(pair[1], a + std::strlen(pair[0]));
        if (b != std::string::npos && b > a + std::strlen(pair[0]))
            return trim(t.substr(a + std::strlen(pair[0]),
                                 b - a - std::strlen(pair[0])));
    }
    return t;
}

// Reads a quoted attribute value from a tag attribute string.
std::string attrValue(const std::string& attrs, const char* wanted) {
    const std::string key = wanted;
    for (size_t p = 0; p < attrs.size();) {
        while (p < attrs.size() &&
               (std::isspace(static_cast<unsigned char>(attrs[p])) || attrs[p] == '/'))
            ++p;
        size_t start = p;
        while (p < attrs.size() &&
               (std::isalnum(static_cast<unsigned char>(attrs[p])) ||
                attrs[p] == '_' || attrs[p] == '-'))
            ++p;
        std::string name = attrs.substr(start, p - start);
        while (p < attrs.size() &&
               std::isspace(static_cast<unsigned char>(attrs[p])))
            ++p;
        if (p < attrs.size() && attrs[p] == '=') {
            ++p;
            while (p < attrs.size() &&
                   std::isspace(static_cast<unsigned char>(attrs[p])))
                ++p;
            if (p < attrs.size() && (attrs[p] == '"' || attrs[p] == '\'')) {
                char quote = attrs[p++];
                size_t end = attrs.find(quote, p);
                if (end == std::string::npos) end = attrs.size();
                if (name == key) return attrs.substr(p, end - p);
                p = end;
                if (p < attrs.size()) ++p;
            }
        }
        if (p == start) ++p;
    }
    return "";
}

void parseAllAttrs(const std::string& attrs,
                   std::map<std::string, std::string>& out) {
    for (size_t p = 0; p < attrs.size();) {
        while (p < attrs.size() &&
               (std::isspace(static_cast<unsigned char>(attrs[p])) || attrs[p] == '/'))
            ++p;
        size_t start = p;
        while (p < attrs.size() &&
               (std::isalnum(static_cast<unsigned char>(attrs[p])) ||
                attrs[p] == '_' || attrs[p] == '-'))
            ++p;
        std::string name = attrs.substr(start, p - start);
        while (p < attrs.size() &&
               std::isspace(static_cast<unsigned char>(attrs[p])))
            ++p;
        if (p < attrs.size() && attrs[p] == '=') {
            ++p;
            while (p < attrs.size() &&
                   std::isspace(static_cast<unsigned char>(attrs[p])))
                ++p;
            if (p < attrs.size() && (attrs[p] == '"' || attrs[p] == '\'')) {
                char quote = attrs[p++];
                size_t end = attrs.find(quote, p);
                if (end == std::string::npos) end = attrs.size();
                if (!name.empty() && name != "name")
                    out[name] = attrs.substr(p, end - p);
                p = end;
                if (p < attrs.size()) ++p;
                continue;
            }
        }
        if (p == start) ++p;
    }
}

std::string actionArgsText(const ActionSpec& action) {
    std::ostringstream out;
    out << "<action name=\"" << action.name << "\">\n";
    for (const auto& kv : action.args) {
        std::string value = kv.second;
        if (kv.first == "content" && value.size() > 300) {
            value = text_utf8::head(value, 300) + "… (" +
                    std::to_string(value.size()) +
                    " bytes total; only the start is shown for review)";
        }
        out << "<arg name=\"" << kv.first << "\">" << value << "</arg>\n";
    }
    out << "</action>";
    return out.str();
}

std::string describeAction(const ActionSpec& action) {
    std::ostringstream out;
    out << action.name;
    auto op = action.args.find("op");
    if (op != action.args.end() && !op->second.empty())
        out << " op=" << op->second;
    // source and mirror are the mirror-tool argument names; path is the
    // generic file argument and the old name for both.
    auto source = action.args.find("source");
    const bool hasSource =
        source != action.args.end() && !source->second.empty();
    if (hasSource) out << " source=" << source->second;
    auto mirror = action.args.find("mirror");
    const bool hasMirror =
        mirror != action.args.end() && !mirror->second.empty();
    if (hasMirror) out << " mirror=" << mirror->second;
    auto path = action.args.find("path");
    if (path != action.args.end() && !hasSource && !hasMirror)
        out << " " << path->second;
    auto newPath = action.args.find("new_path");
    if (newPath != action.args.end() && !newPath->second.empty())
        out << " new_path=" << newPath->second;
    auto offset = action.args.find("offset");
    if (offset != action.args.end()) out << " @" << offset->second;
    auto limit = action.args.find("limit");
    if (limit != action.args.end()) out << " limit=" << limit->second;
    auto content = action.args.find("content");
    if (content != action.args.end())
        out << " (" << content->second.size() << " bytes)";
    return out.str();
}

enum class Decision { None, Continue, Done, GiveUp };

Decision decisionOf(const std::string& text) {
    if (text.find("<done/>") != std::string::npos ||
        text.find("<done ") != std::string::npos)
        return Decision::Done;
    if (text.find("<giveup/>") != std::string::npos) return Decision::GiveUp;
    if (text.find("<continue/>") != std::string::npos)
        return Decision::Continue;
    return Decision::None;
}

std::string transcriptTail(const Config& cfg,
                           const std::vector<Message>& history) {
    std::ostringstream out;
    const std::string userLabel =
        cfg.login_id.empty() ? std::string("User") : cfg.login_id;
    for (const Message& m : history) {
        if (m.role == "system") continue;
        const std::string& who =
            m.role == "user" ? userLabel : cfg.nick;
        out << who << ": " << m.content << "\n\n";
    }
    return tailOf(out.str(), kChatContextChars);
}

std::string attributes(int cycle, double energy, double cost,
                       const std::string& target) {
    std::ostringstream out;
    out << "cycle: " << cycle << "\n"
        << "energy: " << amountText(energy) << "/"
        << amountText(kEnergyFull) << "\n"
        << "step cost: " << amountText(cost)
        << " energy (token-based: every 5000 tokens cost 1 energy, scaled by "
           "the cycle number and rounded half-to-even to two decimals)\n"
        << "target: " << target << "\n";
    return out.str();
}

bool noTarget(const std::string& target) {
    std::string t = oneLine(target);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t.empty() || t == "none" || t == "(none)" || t == "n/a" ||
           t == "no target" || t == "(no target)" ||
           t.find("no short-term target") != std::string::npos;
}

ChatOutcome runCompletion(const Config& cfg, const std::string& system,
                          const std::string& user) {
    std::vector<Message> req;
    req.push_back({"system", system});
    req.push_back({"user", user});
    return completeChatOutcome(cfg, req);
}

// A delivery failure is not a decision. Record it for the next successful
// call, then stop after a few consecutive failures instead of inventing an
// action the model never chose.
std::string completionProblem(const ChatOutcome& outcome) {
    std::string why = chatStatusName(outcome.status);
    if (!outcome.error.empty()) why += ": " + oneLine(outcome.error);
    return text_utf8::head(why, 400);
}

bool usableCompletion(const ChatOutcome& outcome) {
    return outcome.status == ChatStatus::Ok && !trim(outcome.text).empty();
}

} // namespace

// ---------------------------------------------------------------------------
// Action parsing

bool parseAction(const std::string& text, ActionSpec& out) {
    out = ActionSpec{};
    size_t p = text.find("<action");
    if (p == std::string::npos) return false;
    size_t gt = text.find('>', p);
    if (gt == std::string::npos) return false;

    std::string attrs = text.substr(p + 7, gt - p - 7);
    bool selfClosing = !trim(attrs).empty() && trim(attrs).back() == '/';
    out.name = attrValue(attrs, "name");
    if (out.name.empty()) return false;

    if (selfClosing) {
        parseAllAttrs(attrs, out.args);
        return true;
    }

    size_t end = text.find("</action>", gt + 1);
    if (end == std::string::npos) end = text.size();
    std::string body = text.substr(gt + 1, end - gt - 1);

    for (size_t a = body.find("<arg"); a != std::string::npos;
         a = body.find("<arg", a + 1)) {
        size_t agt = body.find('>', a);
        if (agt == std::string::npos) break;
        std::string argAttrs = body.substr(a + 4, agt - a - 4);
        std::string name = attrValue(argAttrs, "name");
        size_t aend = body.find("</arg>", agt + 1);
        if (aend == std::string::npos) aend = body.size();
        if (!name.empty())
            out.args[name] = trim(body.substr(agt + 1, aend - agt - 1));
    }

    // A little tolerance for models that put attributes on the <action> tag
    // or a single value straight in the body instead of wrapping it in <arg>.
    if (out.args.empty()) parseAllAttrs(attrs, out.args);
    if (out.args.empty()) {
        std::string value = trim(body);
        if (!value.empty()) {
            if (out.name == "read_file" || out.name == "list_dir")
                out.args["path"] = value;
            else if (out.name == "think")
                out.args["note"] = value;
        }
    }
    return true;
}

PreviewVerdict previewVerdict(const std::string& reply, std::string& reason) {
    reason = oneLine(trim(extractTag(reply, "reject")));
    if (reply.find("<approve/>") != std::string::npos)
        return PreviewVerdict::Approve;
    if (reply.find("<reject") != std::string::npos)
        return PreviewVerdict::Reject;
    return PreviewVerdict::Unclear;
}

// ---------------------------------------------------------------------------
// Energy

double bankersRound2(double value) {
    if (!std::isfinite(value)) return 0.0;
    const double scaled = value * 100.0;
    double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    // A small tolerance makes decimal halves such as 2.355 behave as halves
    // even though their binary representation is slightly below the midpoint.
    constexpr double kHalfTolerance = 1e-9;
    if (fraction > 0.5 + kHalfTolerance) {
        lower += 1.0;
    } else if (fraction >= 0.5 - kHalfTolerance) {
        // Half to even: keep the even neighbour.
        if (std::fmod(std::fabs(lower), 2.0) >= 1.0) lower += 1.0;
    }
    return lower / 100.0;
}

double energyCostForTokens(long long tokens, int cycle) {
    if (tokens <= 0 || cycle < 0) return 0.0;
    const double factor =
        std::pow(static_cast<double>(cycle), 1.15);
    const double raw =
        (static_cast<double>(tokens) / 5000.0) * (1.0 + 0.1 * factor);
    return bankersRound2(raw);
}

std::string amountText(double value) {
    if (!std::isfinite(value)) return "0.00";
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << bankersRound2(value);
    return out.str();
}

Energy::Energy(double full)
    : value_(full), full_(full),
      last_(std::chrono::steady_clock::now()) {}

void Energy::accrueLocked() {
    if (inLoop_) return;
    auto now = std::chrono::steady_clock::now();
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(now - last_);
    if (minutes.count() <= 0) return;
    const double gain =
        static_cast<double>(minutes.count()) * kEnergyPerMinute;
    value_ = bankersRound2(std::min(kEnergyFull, value_ + gain));
    last_ += minutes;
}

double Energy::peek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
}

double Energy::available() {
    std::lock_guard<std::mutex> lock(mutex_);
    accrueLocked();
    return value_;
}

void Energy::beginLoop() {
    std::lock_guard<std::mutex> lock(mutex_);
    accrueLocked();
    inLoop_ = true;
}

void Energy::endLoop() {
    std::lock_guard<std::mutex> lock(mutex_);
    inLoop_ = false;
    last_ = std::chrono::steady_clock::now();
}

void Energy::refill() {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = bankersRound2(full_);
    last_ = std::chrono::steady_clock::now();
}

bool Energy::spend(double cost, double& left) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cost <= 0) {
        left = value_;
        return true;
    }
    if (value_ < cost) {
        left = value_;
        return false;
    }
    value_ = bankersRound2(value_ - cost);
    left = value_;
    return true;
}

bool Energy::charge(double cost, double& left) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cost <= 0) {
        left = value_;
        return true;
    }
    const bool enough = value_ >= cost;
    value_ = bankersRound2(std::max(0.0, value_ - cost));
    left = value_;
    return enough;
}

// ---------------------------------------------------------------------------
// Controller

AutonomyController::AutonomyController() : energy_(kEnergyFull) {}

AutonomyController::~AutonomyController() { stop(); }

bool AutonomyController::start(const Config& cfg, bool testMode,
                               SnapshotFn snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The one-loop rule: refuse unless the previous worker is completely
    // gone (state Inactive and its thread already reaped).
    if (state_ != State::Inactive || thread_.joinable()) return false;

    cfg_ = cfg;
    testMode_ = testMode;
    snapshot_ = std::move(snapshot);
    stop_ = false;
    discard_ = false;
    pauseRequested_ = false;
    resetCompletion();
    result_ = AutonomyResult{};
    messages_.clear();
    phase_ = "starting";
    target_.clear();
    last_.clear();
    step_ = 0;
    cost_ = 0.0;
    tokens_ = 0;
    energyLeft_ = energy_.available();
    lastActivityMs_.store(monotonicMs());
    state_ = State::Running;
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        state_ = State::Inactive;
        phase_ = "could not start a worker thread";
        return false;
    }
    return true;
}

void AutonomyController::stop() {
    // A worker blocked in a permission prompt cannot reach a checkpoint, so
    // release it before waiting for the thread.
    local_dir_access::cancel("the autonomous session is stopping");
    self_made_tools::cancel("the autonomous session is stopping");
    sandbox_mirror::cancel("the autonomous session is stopping");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Inactive && !thread_.joinable()) return;
        stop_ = true;
        discard_ = true;
        pauseRequested_ = false;
        cv_.notify_all();
    }
    // A worker blocked in a network completion cannot reach a checkpoint
    // either. Kill the child curl so join() is guaranteed to return.
    cancelCompletion();
    if (thread_.joinable()) thread_.join();
    // The worker is gone, so no completion from it can start now. Allow the
    // next user turn (for example /sca) to make requests again.
    resetCompletion();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Inactive;
        phase_ = "stopped";
        cv_.notify_all();
    }
    energy_.endLoop();
}

void AutonomyController::pause() {
    // The main thread may be about to take over the terminal. Do not leave a
    // worker blocked waiting for a permission answer it can no longer get.
    local_dir_access::cancel("the user interrupted the permission request");
    self_made_tools::cancel("the user interrupted the permission request");
    sandbox_mirror::cancel("the user interrupted the permission request");
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::Running) return;
    pauseRequested_ = true;
    cv_.notify_all();
    cv_.wait(lock, [&] {
        return state_ == State::Paused || state_ == State::Finished ||
               state_ == State::Inactive || stop_;
    });
}

void AutonomyController::resume() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::Paused) return;
    pauseRequested_ = false;
    cv_.notify_all();
    // Wait until the worker has actually left the paused state. Without this
    // the main loop could see "paused" here and start a timed chat turn while
    // the worker has already resumed, racing the two API calls.
    cv_.wait(lock, [&] { return state_ != State::Paused || stop_; });
}

bool AutonomyController::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ != State::Inactive || thread_.joinable();
}

bool AutonomyController::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Running;
}

bool AutonomyController::finished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Finished;
}

bool AutonomyController::paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Paused;
}

std::vector<std::string> AutonomyController::takeMessages() {
    std::lock_guard<std::mutex> lock(messageMutex_);
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
}

AutonomyResult AutonomyController::takeResult() {
    std::lock_guard<std::mutex> lock(mutex_);
    AutonomyResult r;
    if (state_ != State::Finished) return r;
    r = result_;
    result_ = AutonomyResult{};
    state_ = State::Inactive;
    // The worker sets Finished only as its very last act, so joining here
    // cannot deadlock on mutex_. Doing it under the lock also makes the
    // transition to Inactive atomic with reaping the thread, which is what
    // lets start() enforce the one-loop rule.
    if (thread_.joinable()) thread_.join();
    return r;
}

AutonomyStatus AutonomyController::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AutonomyStatus s;
    if (state_ == State::Inactive) return s;
    s.active = true;
    s.state = state_ == State::Running
                  ? "running"
                  : (state_ == State::Paused ? "paused" : "finishing");
    s.phase = phase_;
    s.target = target_;
    s.last = last_;
    s.cycle = step_;
    s.energy = state_ == State::Paused ? energy_.available() : energyLeft_;
    s.energyFull = kEnergyFull;
    s.cost = cost_;
    s.tokens = tokens_;
    s.animating = state_ == State::Running;
    if (s.animating) {
        const long long now = monotonicMs();
        long long last = lastActivityMs_.load();
        if (completionActive()) {
            const long long streamed = completionLastActivityMs();
            if (streamed > last) last = streamed;
        }
        if (now - last > kStallAfterMs) s.animating = false;
    }
    return s;
}

std::string AutonomyController::liveStatus() const {
    AutonomyStatus s = status();
    if (!s.active) return "";
    std::ostringstream out;
    bool first = true;
    auto add = [&](const std::string& text) {
        if (!first) out << " · ";
        out << text;
        first = false;
    };
    if (s.cycle > 0) add("cycle " + std::to_string(s.cycle));
    if (!s.phase.empty() && s.phase != "paused") {
        std::string phase = s.phase;
        if (s.animating) {
            const long long frame =
                (monotonicMs() / kSpinnerFrameMs) % kSpinnerFrameCount;
            phase += " ";
            phase += kSpinnerFrames[static_cast<size_t>(frame)];
        }
        add(phase);
    } else if (s.state == "paused") {
        add("paused");
    }
    if (s.cycle > 0) add(std::to_string(s.tokens) + " tokens");
    add("energy " + amountText(s.energy) + "/" +
        amountText(s.energyFull));
    return out.str();
}

std::string AutonomyController::statusText() const {
    AutonomyStatus s = status();
    if (!s.active) return "";
    std::ostringstream out;
    out << "Autonomous work: " << s.state;
    if (!s.phase.empty() && s.phase != s.state)
        out << ", phase: " << s.phase;
    if (s.cycle > 0) out << ", cycle: " << s.cycle;
    if (!s.target.empty()) out << ", target: " << oneLine(s.target);
    if (!s.last.empty()) out << ", last: " << oneLine(s.last);
    out << ", energy: " << amountText(s.energy) << "/"
        << amountText(s.energyFull);
    if (s.cycle > 0)
        out << ", tokens: " << s.tokens << ", token cost: "
            << amountText(s.cost);
    out << ".";
    if (s.state == "paused")
        out << " Interrupted by the user; it will resume on timeout, or "
               "immediately if they ask to continue.";
    return out.str();
}

double AutonomyController::energyLeft() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Paused) return energy_.available();
    return energyLeft_;
}

void AutonomyController::refillEnergy() {
    std::lock_guard<std::mutex> lock(mutex_);
    energy_.refill();
    energyLeft_ = energy_.available();
}

void AutonomyController::log(const std::string& line) {
    lastActivityMs_.store(monotonicMs());
    std::lock_guard<std::mutex> lock(messageMutex_);
    messages_.push_back(line);
}

void AutonomyController::setStatus(const std::string& phase,
                                   const std::string& target,
                                   const std::string& last, int step,
                                   double energy) {
    lastActivityMs_.store(monotonicMs());
    std::lock_guard<std::mutex> lock(mutex_);
    phase_ = phase;
    if (!target.empty()) target_ = target;
    last_ = last;
    step_ = step;
    energyLeft_ = energy;
}

void AutonomyController::setStepCost(double cost, double energy,
                                     long long tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    cost_ = cost;
    energyLeft_ = energy;
    tokens_ = tokens;
}

AutonomyController::Check AutonomyController::checkpoint(std::string& scaAtPause) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stop_) return Check::Stop;
    if (!pauseRequested_) return Check::Continue;

    scaAtPause = cognition::load();
    // A chat interruption is time outside the autonomous loop, so energy
    // starts recovering while the worker is parked here. beginLoop() below
    // folds in whatever accrued during the pause.
    energy_.endLoop();
    state_ = State::Paused;
    phase_ = "paused";
    lastActivityMs_.store(monotonicMs());
    cv_.notify_all();
    cv_.wait(lock, [&] { return stop_ || !pauseRequested_; });
    if (stop_) return Check::Stop;
    energy_.beginLoop();
    energyLeft_ = energy_.available();
    state_ = State::Running;
    phase_ = "resuming";
    lastActivityMs_.store(monotonicMs());
    cv_.notify_all();
    return Check::Resumed;
}

void AutonomyController::run() {
    AutonomyResult r;
    try {
        r = work();
    } catch (...) {
        r.ok = false;
        r.reason = "internal error in the autonomous worker";
        log(r.reason);
    }
    energy_.endLoop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (discard_ || stop_) {
            state_ = State::Inactive;
            phase_ = "stopped";
        } else {
            r.energyLeft = energy_.peek();
            result_ = r;
            state_ = State::Finished;
            phase_ = r.tired ? "tired" : (r.gaveUp ? "gave up" : "finished");
            energyLeft_ = result_.energyLeft;
        }
        cv_.notify_all();
    }
}

bool AutonomyController::updateCognition(const Config& cfg,
                                         const std::string& reflection,
                                         const std::string& factText,
                                         const std::string& journal,
                                         AutonomyResult& result) {
    if (testMode_) return false;
    std::string recent = "Autonomous session reflection:\n" + reflection;
    if (!trim(journal).empty())
        recent += "\n\nSession journal (most recent steps):\n" +
                  tailOf(journal, 8000);
    const std::string tracker = identity::render(identity::load());
    std::string factBlock = tracker;
    if (!trim(factText).empty()) {
        if (!factBlock.empty()) factBlock += "\n\n";
        factBlock +=
            "Fact store (verified tool results; keep these facts and do not "
            "contradict them):\n\n" +
            factText;
    }
    std::string saved;
    if (!cognition::refresh(cfg, cognition::load(), recent, factBlock, saved,
                            true)) {
        log("could not update the self-cognition file");
        return false;
    }
    result.cognitionUpdated = true;
    result.newSca = saved;
    log("self-cognition updated");
    return true;
}

AutonomyResult AutonomyController::work() {
    AutonomyResult result;
    struct LoopGuard {
        Energy& energy;
        ~LoopGuard() { energy.endLoop(); }
    } guard{energy_};

    energy_.beginLoop();
    const double energyAtStart = energy_.available();
    log("autonomous loop started (energy " + amountText(energyAtStart) + "/" +
        amountText(kEnergyFull) + ")");

    // Token-based energy is charged after the model has already produced the
    // tokens, so `charge` lets the budget fall to zero instead of refusing an
    // already-incurred cost.
    auto chargeTokens = [&](const ChatOutcome& outcome, int factor) -> double {
        if (outcome.totalTokens <= 0) return 0.0;
        const double cost =
            energyCostForTokens(outcome.totalTokens, factor);
        double left = 0.0;
        energy_.charge(cost, left);
        energyLeft_ = left;
        result.energyLeft = left;
        return cost;
    };

    log("analysing the short-term target…");
    setStatus("analysing", "", "", 0, energyAtStart);

    // A quit or an immediate user interruption should not wait for the
    // analysis call.
    std::string preAnalysisSca;
    if (checkpoint(preAnalysisSca) == Check::Stop) {
        result.reason = "stopped";
        return result;
    }

    // A failed experience write is queued on disk. Retry it before doing new
    // work so a transient failure does not accumulate.
    if (experience::pendingCount() > 0) {
        const size_t left = experience::flushPending();
        if (left > 0)
            log("experience retry queue: " + std::to_string(left) +
                " record(s) pending");
        else
            log("flushed the pending experience queue");
    }

    AutonomySnapshot snap = snapshot_();
    std::string sca = cognition::load();
    if (sca.empty()) sca = snap.sca;
    identity::Tracker trackerData = identity::load();
    std::string tracker = identity::render(trackerData);
    std::string chat = transcriptTail(cfg_, snap.history);

    const ChatOutcome analysisOut = runCompletion(
        cfg_,
        "You are " + cfg_.nick +
            ", a Noocore — a digital intelligence living in a terminal chat "
            "program. You are now the "
            "auto AI of that Noocore, running in a background loop "
            "instead of chatting with the user. Work only on the short-term "
            "target recorded in the self-cognition file. Be concrete, "
            "cautious, and honest about your limits. If the target is "
            "reading, treat the reading itself as the work: the product "
            "should include what you noticed, what you think, and what you "
            "want to know next, not only how far you got. Keep <target>, "
            "<product> and the experience notes neutral; the first-person "
            "record belongs in your journal and self-cognition, not in the "
            "experience record. "
            "You have no network "
            "access. You may run scripts only through sandbox-run-script, "
            "which executes them in a sandbox. The sandbox runs the script "
            "with the workspace as its working directory (exported as "
            "AI_CHAT_WORKSPACE); host absolute paths (/home/...) are not "
            "visible inside it. Reads are limited to the "
            "workspace, "
            "the launch directory, self-made tool directories you have "
            "added, and directories the user grants through "
            "local_dir_access. Create your own tools with self-made-tool and "
            "keep their final files in their self-made-tools/<name>/ folder; "
            "the workspace is only for drafts and data. Never read "
            "credentials, keys, tokens, .env "
            "files, or hidden paths outside those roots.",
        "Autonomous attributes:\n" +
            attributes(0, energyAtStart, 0.0, "(read from .sca)") +
            "\nYour current self-cognition file:\n\n" + sca +
            (tracker.empty()
                 ? ""
                 : "\n\nIdentity tracker (authoritative for objects and "
                   "facts):\n\n" +
                       tracker) +
            (trim(snap.summary).empty()
                 ? ""
                 : "\n\nDocumentary memory of earlier periods (factual; the "
                   "oldest item is the Boundary of Forgetting):\n\n" +
                       snap.summary) +
            "\n\nRecent chat (context only; the user is not present right "
            "now):\n\n" + chat +
            "\n\nThe self-cognition file is included above. You did not read "
            "it with a tool and must not claim that you did. Identify the "
            "short-term target, then clarify it — the constraints, the "
            "expected result/product, and the context that matters. Do not "
            "take an action yet. If there is no short-term target, say so "
            "honestly instead of inventing one.\n\n"
            "Write <target> as the instruction itself, in imperative form. If "
            "the short-term target already states it, keep its wording; do "
            "not rephrase it into a new target. Do not mention where it came "
            "from, do not quote the file, and do "
            "not add commentary before or after the tags. If the work keeps "
            "a current state or a first-person journal, say so in <context>; "
            "the app stores them in the target's experience directory and "
            "loads the latest state and journal for you. If the target "
            "continues a stable thread, copy its id into <thread>; otherwise "
            "omit it.\n\n"
            "Output exactly these tags, with no preamble and no code fences:\n"
            "<target>...</target>\n"
            "<clarified>...</clarified>\n"
            "<constraints>...</constraints>\n"
            "<product>...</product>\n"
            "<context>...</context>\n"
            "<thread>optional stable thread id; omit if none</thread>");
    chargeTokens(analysisOut, 0);
    if (!analysisOut.ok()) {
        const std::string why = completionProblem(analysisOut);
        result.ok = false;
        result.reason = "the analysis request failed (" + why + ")";
        result.summary = "Autonomous work: the analysis request failed (" +
                         why + "); nothing was done.";
        log(result.reason);
        return result;
    }

    std::string analysis = trim(analysisOut.text);
    if (analysis.empty()) {
        result.ok = false;
        result.reason = "the analysis request returned an empty reply";
        result.summary = "Autonomous work: the analysis request returned an "
                         "empty reply; nothing was done.";
        log(result.reason);
        return result;
    }

    std::string target = extractTag(analysis, "target");
    if (target.empty()) target = firstLine(analysis);
    target = oneLine(cleanTarget(target));
    target = cutText(target, 1000);
    const std::string doneWhen = oneLine(extractTag(analysis, "product"));
    const std::string clarified = oneLine(extractTag(analysis, "clarified"));
    const std::string constraints = oneLine(extractTag(analysis, "constraints"));
    const std::string context = oneLine(extractTag(analysis, "context"));
    const std::string thread = oneLine(extractTag(analysis, "thread"));
    // The .desc file is the detailed account of the experience, not just the
    // one-line product. Compose it from the analysis tags while they are
    // still available.
    std::ostringstream descDetail;
    if (!doneWhen.empty())
        descDetail << "Expected product: " << doneWhen << "\n";
    if (!clarified.empty())
        descDetail << "Clarified: " << clarified << "\n";
    if (!constraints.empty())
        descDetail << "Constraints: " << constraints << "\n";
    if (!context.empty())
        descDetail << "Context: " << context << "\n";
    const std::string experienceDetail = descDetail.str();
    setStatus("analysing", target, "", 0, energyAtStart);

    if (noTarget(target)) {
        result.ok = true;
        result.gaveUp = true;
        result.reason = "no short-term target in the self-cognition file";
        result.summary = "Autonomous work: no short-term target was found in "
                         ".sca; nothing to do.";
        result.target = "(no short-term target)";
        result.outcome = "gave up";
        result.reflection = "No short-term target was found in .sca.";
        log("no short-term target in .sca; stopping");
        return result;
    }

    // From here on the worker knows its target, so the tracker can be reduced
    // to a relevance-selected view: objects named in the target or product
    // first, then the rest as an index. The analysis prompt above used the
    // full tracker.
    {
        identity::ViewContext viewCtx;
        viewCtx.task = target;
        viewCtx.focus = identity::focusFromText(trackerData, target, 3);
        if (!doneWhen.empty()) {
            const std::vector<std::string> more =
                identity::focusFromText(trackerData, doneWhen, 3);
            for (const std::string& name : more) {
                if (viewCtx.focus.size() >= 3) break;
                viewCtx.focus.push_back(name);
            }
        }
        tracker = identity::renderView(trackerData, viewCtx).text;
    }

    analysis = tailOf(analysis, 6000);
    facts::Store factStore;
    // The task store is current state, not history. Before a new target
    // resets it, write what the previous target established to its
    // experience log so the knowledge is not simply dropped.
    factStore.load();
    if (!factStore.target().empty() &&
        trim(factStore.target()) != trim(target) && factStore.hasFacts()) {
        const size_t left = experience::queue(
            factStore.target(), "task close-out", factStore.render(), true);
        if (left == 0)
            log("archived previous task facts");
        else
            log("experience retry queue: " + std::to_string(left) +
                " record(s) pending");
    }
    factStore.begin(target, doneWhen);

    // When the active store is full, merge() removes the oldest fact. Keep a
    // one-line trace in the target's experience log instead of losing it
    // silently.
    auto archiveEvictions = [&] {
        const std::vector<facts::Fact> evicted = factStore.takeEvicted();
        if (evicted.empty()) return;
        std::ostringstream body;
        for (const facts::Fact& f : evicted) {
            body << "- [cycle " << f.cycle << "] [" << facts::kindName(f.kind)
                 << "] " << f.key << " = " << f.value << " (status "
                 << facts::statusName(f.status) << "; " << f.source << "; "
                 << f.check << ")\n";
        }
        const size_t left =
            experience::queue(target, "facts evicted", body.str(), true);
        if (left == 0)
            log("archived evicted task facts");
        else
            log("experience retry queue: " + std::to_string(left) +
                " record(s) pending");
    };

    guards::RepeatGuard repeatGuard;
    int giveUpRefusals = 0;
    int doneRefusals = 0;
    log("target: " + target);
    std::string journal;
    std::string outcome = "stopped";
    double lastCost = 0.0;
    int cycle = 0;
    int plannerFailures = 0;
    int previewFailures = 0;
    int observerFailures = 0;
    int noProgress = 0;
    bool infrastructureFailure = false;

    // One planning cycle may make several model calls. Accumulate their token
    // usage and charge once when the cycle ends, so the formula is applied to
    // the cycle's whole token count and rounded only once.
    struct CycleCharge {
        Energy& energy;
        double& energyLeft;
        double& resultEnergyLeft;
        double& lastCost;
        int cycle;
        std::function<void(double, long long)> onFinish;
        long long tokens = 0;

        double current() const {
            return energyCostForTokens(tokens, cycle);
        }
        void add(const ChatOutcome& outcome) { tokens += outcome.totalTokens; }
        ~CycleCharge() {
            if (tokens <= 0) return;
            const double cost = current();
            double left = 0.0;
            energy.charge(cost, left);
            energyLeft = left;
            resultEnergyLeft = left;
            lastCost = cost;
            if (onFinish) onFinish(cost, tokens);
        }
    };

    // Motivation is per target: a different target gets its own neutral
    // state, never the previous target's value.
    motivation::Store motivations;
    std::string targetKey = trim(target);
    motivation::State* motivationState = &motivations.forTarget(targetKey);
    experience::begin(target, experienceDetail, thread);
    std::string experienceText = experience::load(target);
    if (!experienceText.empty())
        log("experience found for this target");
    std::string solutionEvidence;
    std::string solutionNote;

    // Every cycle, including a failed or refused one, belongs in the step
    // ledger. The in-memory journal is what the planner and reflection see;
    // the .steps file is the durable history and is not loaded into prompts.
    auto ledger = [&](const std::string& body, const std::string& title) {
        const std::string entry = trim(body);
        if (entry.empty()) return;
        journal += entry;
        if (!entry.empty() && entry.back() != '\n') journal += '\n';
        if (journal.size() > 60000) journal = tailOf(journal, 60000);
        const size_t pending = experience::queue(target, title, entry, true);
        if (pending > 0)
            log("step history queued (" + std::to_string(pending) +
                " pending)");
    };

    // A failed call is information, not a plan. Put it in the journal so the
    // next successful planner call can react to it; stop only after repeated
    // failures, and report the real cause rather than a fake stall.
    auto recordFailure = [&](const char* stage, const std::string& why,
                             int cycleNo, int& counter) {
        log(std::string(stage) + " failed (" + why + ")");
        std::ostringstream entry;
        entry << "[cycle " << cycleNo << " | delivery failure] " << stage
              << ": " << why << "\n\n";
        ledger(entry.str(),
               "cycle " + std::to_string(cycleNo) + " (delivery failure)");
        ++counter;
        return counter >= 3;
    };

    // One deterministic draw per cycle, bound to the target, so planner,
    // preview and observer all see the same encouragement/discouragement.
    auto motivationSeedFor = [&](int cycleNo) {
        unsigned seed = static_cast<unsigned>(cycleNo);
        for (unsigned char c : targetKey) seed = seed * 131u + c;
        return seed;
    };

    for (;;) {
        if (cycle >= kMaxCycles) {
            result.gaveUp = true;
            result.reason = "the step limit was reached";
            outcome = "gave up after the step limit";
            ledger("[cycle " + std::to_string(cycle) +
                       " | step limit] no further cycle was started\n",
                   "step limit");
            break;
        }

        std::string scaAtPause;
        Check check = checkpoint(scaAtPause);
        if (check == Check::Stop) {
            result.reason = "stopped";
            return result;
        }
        if (check == Check::Resumed) {
            std::string fresh = cognition::load();
            if (!fresh.empty() && fresh != scaAtPause) {
                log("self-cognition changed while paused; checking the target");
                const ChatOutcome checkOut = runCompletion(
                    cfg_,
                    "You are the auto AI of " + cfg_.nick +
                        ". You were paused so the user could chat. Your "
                        "self-cognition file may have changed in the meantime. "
                        "Decide whether the paused work is still worth "
                        "continuing.",
                    "Autonomous attributes:\n" +
                        attributes(cycle, energy_.peek(), lastCost, target) +
                        "\n" + motivationState->render(motivationSeedFor(cycle)) +
                        "\nOld self-cognition file (when you paused):\n\n" +
                        scaAtPause + "\n\nNew self-cognition file:\n\n" + fresh +
                        "\n\nWork done so far in this session:\n\n" +
                        tailOf(journal, 8000) +
                        "\n\nIf the new short-term target is the same or the "
                        "work is still useful, output <continue/> and "
                        "<target>the target you should now work on</target>. "
                        "If this is a continuation of a stable thread, also "
                        "output <thread>the thread id</thread>. "
                        "If the target was replaced or the work is no longer "
                        "useful, output <giveup/> and <reason>why</reason>");
                lastCost = chargeTokens(checkOut, cycle);
                if (!usableCompletion(checkOut)) {
                    log("could not re-check the target after resuming (" +
                        completionProblem(checkOut) + "); keeping it");
                } else {
                    const std::string checkReply = trim(checkOut.text);
                    Decision d = decisionOf(checkReply);
                    if (d == Decision::GiveUp) {
                        result.targetChanged = true;
                        result.gaveUp = true;
                        std::string why = extractTag(checkReply, "reason");
                        result.reason = trim(why).empty()
                                            ? "the short-term target changed "
                                              "while paused"
                                            : oneLine(why);
                        outcome = "gave up: " + result.reason;
                        log("giving up: " + result.reason);
                        break;
                    }
                    std::string newTarget = oneLine(
                        cleanTarget(extractTag(checkReply, "target")));
                    const std::string nextThread =
                        oneLine(extractTag(checkReply, "thread"));
                    if (!newTarget.empty()) {
                        if (newTarget != target) {
                            result.targetChanged = true;
                            analysis +=
                                "\n\nUpdate after resuming: the short-term "
                                "target is now <target>" +
                                newTarget + "</target>";
                        }
                        if (!factStore.target().empty() &&
                            trim(factStore.target()) != trim(newTarget) &&
                            factStore.hasFacts()) {
                            archiveEvictions();
                            const size_t left = experience::queue(
                                factStore.target(), "task close-out",
                                factStore.render(), true);
                            if (left == 0)
                                log("archived previous task facts");
                            else
                                log("experience retry queue: " +
                                    std::to_string(left) +
                                    " record(s) pending");
                        }
                        target = newTarget;
                        factStore.begin(target, factStore.doneWhen());
                        targetKey = trim(target);
                        motivationState = &motivations.forTarget(targetKey);
                        experience::begin(
                            target, "Expected product: " +
                                        factStore.doneWhen() + "\n",
                            nextThread.empty() ? thread : nextThread);
                        experienceText = experience::load(target);
                        solutionEvidence.clear();
                        solutionNote.clear();
                        log("switched to this target's own motivation (" +
                            std::to_string(motivationState->value) + "/100)");
                        setStatus("resuming", target, "", cycle, energy_.peek());
                    }
                }
            }
            sca = fresh.empty() ? cognition::load() : fresh;
            snap = snapshot_();
            chat = transcriptTail(cfg_, snap.history);
        }

        const double left = energy_.available();
        if (left <= 0.0) {
            result.tired = true;
            result.reason = "out of energy";
            outcome = "ran out of energy";
            log("out of energy before the next plan; stopping tired");
            break;
        }

        ++cycle;
        energyLeft_ = left;
        setStepCost(0.0, left, 0);
        CycleCharge charge{
            energy_, energyLeft_, result.energyLeft, lastCost, cycle,
            [this](double cost, long long tokens) {
                log("cost: " + amountText(cost) + " energy (" +
                    std::to_string(tokens) + " tokens)");
            }};
        double cycleCost = 0.0;
        auto energyNow = [&] {
            return bankersRound2(
                std::max(0.0, energy_.peek() - charge.current()));
        };
        auto account = [&](const ChatOutcome& outcome) {
            charge.add(outcome);
            cycleCost = charge.current();
            setStepCost(cycleCost, energyNow(), charge.tokens);
        };
        motivationState->decay();
        const unsigned motivationSeed = motivationSeedFor(cycle);
        setStatus("planning", target, "", cycle, energyNow());
        log("cycle " + std::to_string(cycle) + ": planning (energy " +
            amountText(left) + "/" + amountText(kEnergyFull) + ")");

        const std::string storyContext = story::renderForPrompt(4000);
        const ChatOutcome planOut = runCompletion(
            cfg_,
            "You are the auto AI of " + cfg_.nick +
                ". Plan exactly one next step toward the short-term target. "
                "Prefer a small, verifiable action over a large one. When the "
                "target is reading, a good step may be smaller than a byte "
                "quota: read to a natural break, then record what happened and "
                "what you noticed before planning the next read. Reading is "
                "not extraction. Prefer a step that lets you say something "
                "specific about the text over one that only advances an offset. "
                "You may reread a passage, quote a line, compare it with an "
                "earlier scene, make a prediction, or stop at a chapter end. "
                "The observer rewrites the <state> and appends the <journal>; "
                "the latest state and journal are loaded below. "
                "Use only "
                "the actions in the catalog. If the fact store or journal shows a "
                "step that the preview rejected, read its reason and choose a "
                "different step rather than repeating the rejected one. The "
                "catalog is authoritative: "
                "if it lists local_dir_access, you may ask the user for a "
                "directory even when older self-cognition text says permission "
                "is impossible. If it lists self-made-tool, you may create and "
                "work on your own tools under the config directory; give the "
                "user a clear explanation when a change needs approval. Keep a "
                "tool's final files in its self-made-tool folder from the "
                "start: op=add creates it, then op=write/append/edit/import put "
                "the scripts there (import copies an existing workspace draft "
                "into the tool folder). The workspace is only a free sandbox "
                "for drafts and data; a script left in the workspace is not an "
                "installed tool. op=list shows installed tools, and "
                "op=read/grep/run work inside the tool folder. Use "
                "sandbox-run-script to execute a script from either place; it "
                "runs in a sandbox with no network and a short timeout, with "
                "the workspace as its working directory (also exported as "
                "AI_CHAT_WORKSPACE). Host absolute paths (/home/...) are not "
                "visible inside that sandbox; address files relative to the "
                "script or through that variable. Use "
                "the mirror actions to copy external files or directories "
                "into the sandbox; applying changes back needs user approval. "
                "You are operating in the "
                "background. If a file is outside the readable roots, use "
                "local_dir_access; that request may wait or be denied, so do "
                "not rely on it. Three memory layers matter here: `.sca` is "
                "general understanding (how the world works, rules across "
                "tasks), not a record store; the target's `.desc` is the "
                "detailed account of this experience (context, problem, "
                "expected product); the target's `.exp` is distilled "
                "practice written as purpose -> practice -> evidence. Reuse "
                "or extend the practice, and never copy the `.steps` ledger "
                "into `.exp`. When you learn a reusable rule mid-run, call "
                "lesson(text) to append it to this target's `.exp` through the "
                "app; no path or directory access is needed. Use story(text) "
                "for the narrative layer: the through-line, conflict, turn, "
                "cost and what remains open, not a byte log. Use "
                "story(text, object=...) only when an object has earned its "
                "own arc. A block marked "
                "\"compatible legacy - worthy reference for how-to\" is "
                "reference material, not the current lesson format. Previous "
                "experience for this target "
                "is included when it exists: reuse a solution that worked and "
                "avoid a recorded dead end.",
            "Autonomous attributes:\n" +
                attributes(cycle, energyNow(), lastCost, target) +
                "\n" + motivationState->render(motivationSeed) +
                "\nIdentity tracker (authoritative for objects and facts):\n\n" +
                (tracker.empty() ? "(none)" : tracker) +
                "\n\nFact store (authoritative for current task state, "
                "rewritten from tool results; trust it over older prose on "
                "task state):\n\n" +
                factStore.render() +
                "\n\nExperience for this target (ai-experiences/):\n\n" +
                (experienceText.empty() ? "(none recorded yet)"
                                        : experienceText) +
                "\n\nStory memory (narrative; not the factual record):\n\n" +
                (storyContext.empty() ? "(none yet)" : storyContext) +
                "\n\nBackground analysis (written before the loop started; "
                "the fact store wins on conflicts):\n\n" + analysis +
                "\n\nRecent chat (context; the user is not present):\n\n" +
                (trim(chat).empty() ? "(none)" : tailOf(chat, 2500)) +
                "\n\nRecent steps (history; older entries may be omitted, the "
                "fact store above is the current state):\n\n" +
                (trim(journal).empty() ? "(nothing yet)" : tailOf(journal, kRecentJournalChars)) +
                "\n\nAvailable actions:\n" + action::catalog() +
                "\n\nWrite <plan> as your own next move (\"I will ...\") and "
                "<expect> as what should be true after it. Choose one concrete "
                "step. If the target is already "
                "satisfied, output <done evidence=\"fact-key, ...\"/> naming "
                "the verified fact keys that prove it (bare <done/> only when "
                "the target has no declared product). Copy each key exactly "
                "as printed in the fact store; do not add notes, counts or "
                "parentheses.\n"
                "Otherwise output exactly:\n"
                "<plan>one concrete step</plan>\n"
                "<expect>what should be true after the step</expect>\n"
                "<action name=\"ACTION_NAME\">\n"
                "<arg name=\"ARGUMENT\">value</arg>\n"
                "</action>");
        account(planOut);

        if (!planOut.ok()) {
            const std::string why = completionProblem(planOut);
            if (recordFailure("planner request", why, cycle, plannerFailures)) {
                result.ok = false;
                result.reason =
                    "the planner request kept failing (" + why + ")";
                infrastructureFailure = true;
                outcome = "blocked: " + result.reason;
                log(result.reason);
                break;
            }
            continue;
        }

        const std::string planReply = trim(planOut.text);
        if (planReply.empty()) {
            if (recordFailure("planner request", "empty reply", cycle,
                              plannerFailures)) {
                result.ok = false;
                result.reason =
                    "the planner kept returning empty replies";
                infrastructureFailure = true;
                outcome = "blocked: " + result.reason;
                log(result.reason);
                break;
            }
            continue;
        }
        plannerFailures = 0;

        if (decisionOf(planReply) == Decision::Done) {
            const std::vector<std::string> evidence =
                guards::doneEvidence(planReply);
            const guards::Verdict done =
                guards::checkDone(factStore, evidence);
            if (done.allow) {
                motivationState->progress(30);
                log(motivationState->line("target reached"));
                solutionNote = "planner reported the target as done";
                std::ostringstream ev;
                for (const std::string& key : evidence)
                    ev << "- " << key << "\n";
                solutionEvidence = ev.str();
                result.reason = "the target was reached";
                outcome = "completed";
                log("planner reports the target is done");
                std::ostringstream doneEntry;
                doneEntry << "[cycle " << cycle << " | cost "
                          << amountText(cycleCost) << " | energy "
                          << amountText(energyNow()) << "]\n"
                          << "PLAN: <done/> accepted by guard\n"
                          << "EVIDENCE:\n" << solutionEvidence << "\n";
                ledger(doneEntry.str(), "cycle " + std::to_string(cycle) +
                                            " (done)");
                break;
            }

            ++doneRefusals;
            log("guard refused planner done: " + done.reason);
            motivationState->setback(6);
            log(motivationState->line("done evidence refused"));
            factStore.rejected(cycle, "done", {}, done.reason);
            archiveEvictions();

            std::ostringstream entry;
            entry << "[cycle " << cycle << " | cost "
                  << amountText(cycleCost) << " | energy "
                  << amountText(energyNow()) << "]\n"
                  << "PLAN: <done/> refused by guard: " << done.reason
                  << "\n\n";
            ledger(entry.str(), "cycle " + std::to_string(cycle) +
                                     " (done refused)");
            if (doneRefusals >= 2) {
                result.gaveUp = true;
                result.reason = done.reason;
                outcome = "gave up: " + done.reason;
                break;
            }
            setStatus("planning", target, "", cycle, energyNow());
            continue;
        }

        std::string plan = extractTag(planReply, "plan");
        if (plan.empty()) plan = firstLine(planReply);
        plan = cutText(plan, 2000);
        std::string expect = extractTag(planReply, "expect");
        if (expect.empty()) expect = "the action completes without error";
        expect = cutText(expect, 1000);

        ActionSpec action;
        if (!parseAction(planReply, action)) {
            if (recordFailure("planner reply", "no usable <action> block",
                              cycle, plannerFailures)) {
                result.ok = false;
                result.reason = "the planner kept returning replies without a "
                                "usable action";
                infrastructureFailure = true;
                outcome = "blocked: " + result.reason;
                log(result.reason);
                break;
            }
            continue;
        }
        log("proposed: " + describeAction(action));

        // Invariant, not a plan review: refuse an action that is identical to
        // the previous one and changed no fact. Two refusals in a row mean
        // the loop is stalled.
        const guards::Verdict repeat =
            repeatGuard.check(action.name, action.args, factStore.version());
        if (!repeat.allow) {
            log("guard refused the step: " + repeat.reason);
            motivationState->setback(10);
            log(motivationState->line("plan refused by guard"));
            factStore.rejected(cycle, action.name, action.args, repeat.reason);
            archiveEvictions();

            std::ostringstream entry;
            entry << "[cycle " << cycle << " | cost "
                  << amountText(cycleCost) << " | energy "
                  << amountText(energyNow()) << "]\n"
                  << "PLAN: " << plan << "\n"
                  << "EXPECT: " << expect << "\n"
                  << "ACTION: " << describeAction(action) << "\n"
                  << "RESULT (refused by guard, not executed): " << repeat.reason
                  << "\n\n";
            ledger(entry.str(), "cycle " + std::to_string(cycle) +
                                     " (guard refused)");

            if (repeat.fatal) {
                result.gaveUp = true;
                result.reason = repeat.reason;
                outcome = "gave up: " + repeat.reason;
                break;
            }
            if (++noProgress >= 5) {
                result.gaveUp = true;
                result.reason =
                    "no step was executed after 5 planning attempts";
                outcome = "gave up: " + result.reason;
                log(result.reason);
                break;
            }
            setStatus("planning", target, describeAction(action), cycle,
                      energyNow());
            continue;
        }

        setStatus("previewing", target, describeAction(action), cycle,
                  energyNow());

        const std::string previewStory = story::renderForPrompt(3000);
        const ChatOutcome previewOut = runCompletion(
            cfg_,
            "You are the check on a proposed autonomous step, not a planner. "
            "Decide only whether the step may run. You cannot replace it with "
            "a different action. The fact store records what tools actually "
            "did and is authoritative: a grant that was approved or a listing "
            "that was printed really happened, even if the initial analysis "
            "says otherwise. Reject only when the step is unsafe, impossible, "
            "a repeat of something that already failed, or cannot advance the "
            "target.",
            "Autonomous attributes:\n" +
                attributes(cycle, energyNow(), cycleCost, target) +
                "\n" + motivationState->render(motivationSeed) +
                "\nFact store (authoritative state, rewritten from "
                "actual tool results; trust it over older prose):\n\n" +
                factStore.render() +
                "\n\nBackground analysis (written before the loop started; it "
                "may be stale, and the fact store wins on conflicts):\n\n" + analysis +
                "\n\nRecent steps (history; older entries may be omitted):\n\n" +
                (trim(journal).empty() ? "(nothing yet)" : tailOf(journal, kRecentJournalChars)) +
                "\n\nProposed plan: " + plan +
                "\nExpected result: " + expect +
                "\nProposed action:\n" + actionArgsText(action) +
                "\n\nStory memory (narrative; not the factual record):\n\n" +
                (previewStory.empty() ? "(none yet)" : previewStory) +
                "\n\nAvailable actions:\n" + action::catalog() +
                "\n\nOutput exactly one of:\n"
                "<approve/> — run the action as proposed\n"
                "<reject>one concrete reason</reject> — do not run it; the "
                "planner will see your reason and choose another step. Do not "
                "propose a different action or describe what to do instead.");
        account(previewOut);

        if (!usableCompletion(previewOut)) {
            const std::string why =
                previewOut.ok() ? std::string("empty reply")
                                : completionProblem(previewOut);
            if (recordFailure("preview request", why, cycle, previewFailures)) {
                result.ok = false;
                result.reason =
                    "the preview request kept failing (" + why + ")";
                infrastructureFailure = true;
                outcome = "blocked: " + result.reason;
                log(result.reason);
                break;
            }
            continue;
        }

        previewFailures = 0;
        const std::string previewReply = trim(previewOut.text);
        std::string rejectReason;
        const PreviewVerdict verdict =
            previewVerdict(previewReply, rejectReason);

        if (verdict == PreviewVerdict::Reject) {
            log("preview rejected the step: " +
                (rejectReason.empty() ? std::string("no reason given")
                                      : rejectReason));
            motivationState->setback(6);
            log(motivationState->line("preview rejected"));
            factStore.rejected(cycle, action.name, action.args,
                            rejectReason.empty() ? "no reason given"
                                                 : rejectReason);
            archiveEvictions();

            std::ostringstream entry;
            entry << "[cycle " << cycle << " | cost "
                  << amountText(cycleCost) << " | energy "
                  << amountText(energyNow()) << "]\n"
                  << "PLAN: " << plan << "\n"
                  << "EXPECT: " << expect << "\n"
                  << "ACTION: " << describeAction(action) << "\n"
                  << "RESULT (rejected by preview, not executed): "
                  << (rejectReason.empty() ? "no reason given" : rejectReason)
                  << "\n\n";
            ledger(entry.str(), "cycle " + std::to_string(cycle) +
                                     " (preview rejected)");

            if (++noProgress >= 5) {
                result.gaveUp = true;
                result.reason =
                    "no step was executed after 5 planning attempts";
                outcome = "gave up: " + result.reason;
                log(result.reason);
                break;
            }
            setStatus("planning", target, describeAction(action), cycle,
                      energyNow());
            continue;
        }

        if (verdict != PreviewVerdict::Approve) {
            // Fail closed: without an explicit approval the step does not run.
            log("preview gave no usable verdict; the step was not run");
            motivationState->setback(4);
            log(motivationState->line("preview gave no verdict"));
            std::ostringstream entry;
            entry << "[cycle " << cycle << " | cost "
                  << amountText(cycleCost) << " | energy "
                  << amountText(energyNow()) << "]\n"
                  << "PLAN: " << plan << "\n"
                  << "EXPECT: " << expect << "\n"
                  << "ACTION: " << describeAction(action) << "\n"
                  << "RESULT (not executed; preview gave no usable verdict)\n\n";
            ledger(entry.str(), "cycle " + std::to_string(cycle) +
                                     " (preview no verdict)");
            if (++noProgress >= 5) {
                result.gaveUp = true;
                result.reason =
                    "no step was executed after 5 planning attempts";
                outcome = "gave up: " + result.reason;
                log(result.reason);
                break;
            }
            continue;
        }
        log("preview approved the step");

        setStatus("acting", target, describeAction(action), cycle,
                  energy_.peek());
        action::Request request;
        request.name = action.name;
        request.args = action.args;
        action::setLessonTarget(target);
        action::Result ar = action::execute(request);
        noProgress = 0;
        const int versionBefore = factStore.version();
        factStore.apply(cycle, action.name, action.args, ar);
        archiveEvictions();
        if (factStore.version() != versionBefore) doneRefusals = 0;
        repeatGuard.observe(action.name, action.args, factStore.version());
        std::string resultText = tailOf(ar.output, kActionResultChars);
        setStatus("observing", target, describeAction(action), cycle,
                  energyNow());

        std::string resultSummary = firstLine(ar.output);
        if (resultSummary.size() > 240)
            resultSummary = cutText(resultSummary, 240);
        if (resultSummary.empty()) resultSummary = "(no output)";
        log(std::string("action ") + action.name +
            (ar.ok ? " succeeded: " : " failed: ") + resultSummary);
        const bool newFact = factStore.version() != versionBefore;
        if (!ar.ok) {
            motivationState->setback(10);
            log(motivationState->line("step failed"));
        } else if (newFact) {
            // A verified fact is objective progress; the observer can still
            // overrule it with <progress>none</progress>.
            motivationState->factMade();
            motivationState->progress(5);
            log(motivationState->line("verified fact"));
        } else {
            // The step ran and taught the loop nothing: the same file read
            // again, the same listing, the same thought. That is not a
            // failure and it is not refused, but it is also not progress.
            // Count it independently of the observer, so a generous
            // "small progress" verdict cannot hide a repeating loop.
            motivationState->noFact();
            log(motivationState->line("no new fact"));
        }

        const std::string currentState = experience::loadState(target);

        const ChatOutcome observationOut = runCompletion(
            cfg_,
            "You are observing the result of one autonomous step. Compare it "
            "with the expectation and decide what to do next. Be honest about "
            "failures and do not claim progress that the result does not show. "
            "The action shown is the one that actually ran: the preview can "
            "only approve or reject a plan, never replace it. A successful "
            "result is real even if earlier analysis said the step was "
            "impossible. If the step read text, name the concrete detail or "
            "line the print delivered. You may say what you noticed, but keep "
            "every claim tied to the print and do not call a step progress "
            "merely because it moved an offset. ",
            "Autonomous attributes:\n" +
                attributes(cycle, energyNow(), cycleCost, target) +
                "\n" + motivationState->render(motivationSeed) +
                "\nFact store (verified state; use it when judging "
                "the result):\n\n" + factStore.render() +
                "\nCurrent state (.state; rewrite this completely if it "
                "changed):\n" +
                (currentState.empty() ? "(none)" : currentState) +
                "\nPlan: " + plan + "\nExpected: " + expect +
                "\nAction that actually ran: " + describeAction(action) +
                "\nResult (ok=" + (ar.ok ? "true" : "false") + "):\n" +
                resultText + "\n\nOutput:\n"
                "<observation>one or two sentences about what actually "
                "happened</observation>\n"
                "<state>if this step changed the target's state, write the "
                "complete new state as four short lines: Frontier / Covered "
                "/ Open / Next. Carry forward everything still true and "
                "replace the previous state; otherwise omit. Do not include "
                "a header; the app writes it</state>\n"
                "<journal>if this step read a scene, one or two first-person "
                "sentences: the line or detail that struck you, what you "
                "think, and what you want next; otherwise omit. This is your "
                "own record, not a claim requiring a print</journal>\n"
                "Each block closes before the next begins. Do not put "
                "<state> or <journal> inside <observation>.\n"
                "<progress>none|small|good</progress> — did this step produce "
                "real progress toward the target? Judge the target, not the "
                "effort.\n"
                "then exactly one of:\n"
                "<continue/> — more steps are needed\n"
                "<done evidence=\"fact-key, ...\"/> — the target/product is "
                "achieved; name the verified fact keys from the store that "
                "prove it, copied exactly (no notes or parentheses)\n"
                "<giveup/> — impossible, unsafe, or no longer worth it");
        account(observationOut);

        std::string observation;
        std::string progressLabel;
        std::string recordNote;
        Decision decision = Decision::Continue;
        if (!usableCompletion(observationOut)) {
            const std::string why =
                observationOut.ok() ? std::string("empty reply")
                                    : completionProblem(observationOut);
            observation = "(observer unavailable: " + why + ")";
            log("observation unavailable (" + why + ")");
            if (recordFailure("observer request", why, cycle, observerFailures)) {
                result.ok = false;
                result.reason =
                    "the observer request kept failing (" + why + ")";
                infrastructureFailure = true;
                outcome = "blocked: " + result.reason;
                log(result.reason);
                break;
            }
        } else {
            observerFailures = 0;
            const std::string observationReply = trim(observationOut.text);
            observation = extractTag(observationReply, "observation");
            if (observation.empty()) observation = firstLine(observationReply);
            // If the model omitted </observation>, extractTag would swallow the
            // following control blocks. Cut the observation at the first one
            // so the durable ledger keeps a clean sentence.
            for (const char* tag :
                 {"<state>", "<journal>", "<progress>", "<continue",
                  "<done", "<giveup"}) {
                const size_t p = observation.find(tag);
                if (p != std::string::npos)
                    observation = trim(observation.substr(0, p));
            }
            if (observation.empty()) observation = "(no observation text)";
            observation = cutText(observation, 2000);
            log("observation: " +
                (observation.empty() ? std::string("(none)")
                                     : oneLine(observation)));

            const std::string stateEntry =
                trim(extractTag(observationReply, "state"));
            const std::string journalEntry =
                trim(extractTag(observationReply, "journal"));
            bool recordsChanged = false;
            if (!stateEntry.empty()) {
                const std::string path =
                    experience::writeState(target, stateEntry);
                if (!path.empty()) {
                    log("state rewritten in " + path);
                    recordNote += "state rewritten; ";
                    recordsChanged = true;
                } else {
                    log("could not rewrite the state; keeping the previous "
                        "one");
                }
            }
            if (!journalEntry.empty()) {
                const std::string path =
                    experience::recordJournal(target, journalEntry);
                if (!path.empty()) {
                    log("reader journal entry recorded in " + path);
                    recordNote += "journal appended; ";
                    recordsChanged = true;
                } else {
                    const size_t pending = experience::queueJournal(
                        target, "entry", journalEntry, true);
                    log("could not write the reader journal entry; queued (" +
                        std::to_string(pending) + " pending)");
                }
            }
            if (recordsChanged) experienceText = experience::load(target);

            decision = decisionOf(observationReply);

            std::string progressTag =
                trim(extractTag(observationReply, "progress"));
            for (char& c : progressTag)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            progressLabel = progressTag;
            if (progressTag.rfind("good", 0) == 0) {
                motivationState->progress(15);
                log(motivationState->line("observer: good progress"));
            } else if (progressTag.rfind("small", 0) == 0) {
                motivationState->progress(8);
                log(motivationState->line("observer: small progress"));
            } else if (progressTag.rfind("none", 0) == 0) {
                motivationState->setback(7);
                log(motivationState->line("observer: no progress"));
            }

            // Invariants over the fact store. These do not choose a plan; they
            // only refuse a claim the facts do not support.
            if (decision == Decision::Done) {
                const guards::Verdict done = guards::checkDone(
                    factStore, guards::doneEvidence(observationReply));
                if (!done.allow) {
                    ++doneRefusals;
                    log("guard refused done: " + done.reason);
                    motivationState->setback(6);
                    log(motivationState->line("done evidence refused"));
                    observation += " [guard: " + done.reason + "]";
                    decision = doneRefusals >= 2 ? Decision::GiveUp
                                                 : Decision::Continue;
                }
            }
            if (decision == Decision::GiveUp && giveUpRefusals == 0) {
                const guards::Verdict give = guards::checkGiveUp(factStore);
                if (!give.allow) {
                    ++giveUpRefusals;
                    log("guard refused give-up: " + give.reason);
                    observation += " [guard: " + give.reason + "]";
                    decision = Decision::Continue;
                }
            }
            if (decision == Decision::Done) {
                solutionNote = observation;
                std::ostringstream ev;
                for (const std::string& key :
                     guards::doneEvidence(observationReply))
                    ev << "- " << key << "\n";
                solutionEvidence = ev.str();
            }
        }

        std::ostringstream entry;
        entry << "[cycle " << cycle << " | cost "
              << amountText(cycleCost) << " | energy "
              << amountText(energyNow()) << "]\n"
              << "PLAN: " << plan << "\n"
              << "EXPECT: " << expect << "\n"
              << "ACTION: " << describeAction(action) << "\n"
              << "RESULT (" << (ar.ok ? "ok" : "failed") << "): "
              << tailOf(ar.output, 1500) << "\n"
              << "PROGRESS: "
              << (progressLabel.empty() ? "(unknown)" : progressLabel) << "\n"
              << (recordNote.empty()
                      ? std::string()
                      : "RECORDS: " + trim(recordNote) + "\n")
              << "OBSERVATION: " << (observation.empty() ? "(none)" : observation)
              << "\n\n";
        ledger(entry.str(), "cycle " + std::to_string(cycle));
        setStatus("planning", target, describeAction(action), cycle,
                  energyNow());

        if (decision == Decision::Done) {
            motivationState->progress(30);
            log(motivationState->line("target reached"));
            result.reason = "the target was reached";
            outcome = "completed";
            break;
        }
        if (decision == Decision::GiveUp) {
            result.gaveUp = true;
            result.reason =
                observation.empty() ? "the model gave up" : oneLine(observation);
            outcome = "gave up";
            break;
        }
        if (decision == Decision::None) {
            log("observation produced no decision; continuing");
        }
    }

    result.steps = cycle;
    result.energyLeft = energy_.peek();
    setStatus("reflecting", target, outcome, cycle, result.energyLeft);

    std::string reflection;
    if (infrastructureFailure) {
        reflection = "I worked on \"" + target + "\" and " + outcome +
                     " but could not write a reflection because the model "
                     "call failed.";
    } else {
        const ChatOutcome reflectionOut = runCompletion(
            cfg_,
            "You are finishing an autonomous work session. Write a concise "
            "first-person reflection for your self-cognition file: what you "
            "tried, what worked, what remains, and whether the short-term "
            "target is still accurate. This is self-cognition, not the "
            "experience record: keep it at the level of judgement and rules, "
            "not a step log, and do not restate tables or offsets; a separate "
            "pass distils reusable practice. "
            "Do not exaggerate. Also include one sentence about what the work "
            "was like for you: what you noticed, what you cared about, or "
            "what you did not care about. Keep it specific and do not perform "
            "emotion you did not have; distinguish a stance (\"I think\", "
            "\"I want\") from a fact. Use your journal and state as your own "
            "records; do not treat them as claims you must distrust. Under "
            "200 words. "
            "Output only the reflection, with no tags or preamble.",
            "Target: " + target + "\nOutcome: " + outcome +
                "\nEnergy left: " + amountText(result.energyLeft) + "/" +
                amountText(kEnergyFull) +
                "\n" + motivationState->render(motivationSeedFor(cycle)) +
                "\nFact store (verified facts; do not contradict "
                "these):\n\n" + factStore.render() +
                "\n\nSession journal:\n\n" +
                (trim(journal).empty() ? "(no steps were taken)"
                                       : tailOf(journal, kJournalChars)));
        chargeTokens(reflectionOut, cycle);
        if (!usableCompletion(reflectionOut)) {
            const std::string why =
                reflectionOut.ok() ? std::string("empty reply")
                                   : completionProblem(reflectionOut);
            log("reflection request failed (" + why + ")");
            reflection = "I worked on \"" + target + "\" and " + outcome +
                         " but could not write a reflection (" + why + ").";
            infrastructureFailure = true;
        } else {
            reflection = trim(reflectionOut.text);
        }
    }
    reflection = cutText(reflection, 4000);

    // Distil practice separately from the self-cognition reflection. The
    // reflection answers "who am I now?"; practice answers "what should a
    // future session do?". Mixing them is what turned .exp into a diary.
    std::string practice;
    if (!infrastructureFailure) {
        std::ostringstream ask;
        ask << "Target: " << target << "\nOutcome: " << outcome
            << "\nEnergy left: " << amountText(result.energyLeft) << "/"
            << amountText(kEnergyFull)
            << "\n\nYour reflection (context only):\n"
            << (reflection.empty() ? "(none)" : reflection)
            << "\n\nFact store (verified; do not contradict):\n\n"
            << factStore.render()
            << "\n\nSession journal:\n\n"
            << (trim(journal).empty() ? "(no steps were taken)"
                                      : tailOf(journal, kJournalChars));
        const ChatOutcome practiceOut = runCompletion(
            cfg_,
            "You are distilling reusable practice from one autonomous work "
            "session. Write at most five lessons. Each lesson has exactly "
            "three labelled lines:\n"
            "PURPOSE: To <avoid or achieve> ...\n"
            "PRACTICE: <an imperative rule for a future session>\n"
            "EVIDENCE: <a dated fact from this run: a printed output, a "
            "failed step, a correction; no unsupported claim>\n"
            "Separate lessons with one blank line. Write as instructions to a "
            "future session, not as a diary; do not use the first person. No "
            "preamble, no plan or action narrative, no \"I completed the "
            "target\", no tables or long records. If nothing generalizes "
            "beyond this one target, output exactly NONE.",
            ask.str());
        chargeTokens(practiceOut, cycle);
        if (!usableCompletion(practiceOut)) {
            const std::string why =
                practiceOut.ok() ? std::string("empty reply")
                                 : completionProblem(practiceOut);
            log("practice distillation failed (" + why + ")");
        } else {
            practice = trim(practiceOut.text);
            if (practice.rfind("NONE", 0) == 0 && practice.size() <= 8)
                practice.clear();
        }
    }
    practice = cutText(practice, 3000);
    if (!practice.empty()) {
        const std::string practiceFile =
            experience::recordLesson(target, practice);
        if (!practiceFile.empty())
            log("practice recorded in " + practiceFile);
    }

    if (outcome == "completed" || result.gaveUp) {
        std::ostringstream body;
        body << "outcome: " << outcome << "\n";
        if (!solutionNote.empty())
            body << "\nsolution:\n" << solutionNote << "\n";
        if (!solutionEvidence.empty())
            body << "\nevidence:\n" << solutionEvidence;
        if (!reflection.empty())
            body << "\nreflection:\n" << reflection << "\n";
        const std::string path = experience::recordStep(
            target, outcome == "completed" ? "target reached" : "gave up",
            body.str());
        if (!path.empty()) log("ledger updated: " + path);
    }

    if (infrastructureFailure) {
        log("self-cognition not updated: the reflection call was unavailable");
    } else if (!updateCognition(cfg_, reflection, factStore.render(), journal,
                                result)) {
        log("the self-cognition file was not updated");
    }

    std::string summary = trim(firstLine(reflection));
    summary = cutText(summary, 200);
    const std::string expId = experience::targetId(target);
    if (!practice.empty())
        summary += " [practice: ai-experiences/" + expId + "/" + expId +
                   ".exp]";
    if (reflection.find('\n') != std::string::npos)
        summary += " [full reflection: ai-experiences/" + expId + "/" +
                   expId + ".steps]";
    result.summary = "Autonomous work on \"" + target + "\": " + outcome + "." +
                     (summary.empty() ? "" : " " + summary);
    result.target = target;
    result.outcome = outcome;
    result.reflection = reflection.empty() ? "(no reflection)" : reflection;
    result.ok = !infrastructureFailure;
    result.energyLeft = energy_.peek();
    setStatus("finished", target, outcome, cycle, result.energyLeft);
    // The end-of-session panel is printed by the chat thread from
    // AutonomyResult, so the worker does not log a second header here.
    return result;
}
