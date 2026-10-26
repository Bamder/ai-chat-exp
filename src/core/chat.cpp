#include "aichat/core/chat.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "aichat/io/json.h"
#include "aichat/core/sentence.h"
#include "aichat/ui/ui.h"
#include "aichat/core/util.h"
#include "ai-tools/sse-watch/sse-watch.h"
#include "ai-tools/text-utf8/text-utf8.h"

extern char** environ;

namespace {

struct TempFile {
    std::string path;
    ~TempFile() { if (!path.empty()) ::unlink(path.c_str()); }
};

TempFile makeTemp(const std::string& prefix, const std::string& body) {
    std::string tmpl = "/tmp/" + prefix + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    TempFile tf;
    int fd = mkstemp(buf.data());
    if (fd < 0) return tf;
    tf.path = buf.data();
    size_t off = 0;
    while (off < body.size()) {
        ssize_t w = write(fd, body.data() + off, body.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    close(fd);
    return tf;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return trim(ss.str());
}

int exitCodeOf(int status) {
    if (status < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// curl exit codes that mean the connection broke (DNS/connect/TLS/send/recv,
// timeouts, HTTP/2 stream errors) rather than the server rejecting the
// request. Those are worth retrying: providers occasionally close a stream
// before sending a single byte.
bool transientCurlError(int rc) {
    switch (rc) {
        case -1:
        case 6:  // couldn't resolve host
        case 7:  // couldn't connect
        case 16: // HTTP/2 framing error
        case 18: // partial transfer
        case 28: // operation timed out
        case 52: // empty reply from server
        case 55: // send error
        case 56: // recv error (e.g. unexpected TLS EOF)
        case 92: // HTTP/2 stream error
            return true;
        default:
            return false;
    }
}

// One completion can be running at a time, and the quit path may need to stop
// it from another thread. The pid stays registered until it is reaped, so it
// cannot be recycled between a cancel and waitpid(). Cancellation is a latch:
// once requested, no new attempt starts until resetCompletion().
std::mutex g_completionMutex;
pid_t g_completionPid = -1;
bool g_completionCancelled = false;

// The live prompt reads these without taking the completion mutex.
std::atomic<bool> g_completionActive{false};
std::atomic<long long> g_completionLastActivityMs{0};
std::atomic<long long> g_completionTokensSeen{0};

// A stream that stays connected but sends no token for this long is treated
// as stalled. Keep-alive comment lines do not count: they prove the socket is
// alive, not that the model is producing. The first-token budget is larger
// because a long prompt has to be prefilled before any output exists.
//
// The two values can be overridden in milliseconds for a provider that
// legitimately pauses longer, and for the selftest's fake stream:
//   AI_CHAT_FIRST_TOKEN_MS, AI_CHAT_TOKEN_GAP_MS
long long stallMsFromEnv(const char* name, long long fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    errno = 0;
    const long long n = std::strtoll(v, &end, 10);
    if (errno != 0 || end == v || n <= 0) return fallback;
    return n;
}

const long long kFirstTokenDefaultMs = 180000; // 3 minutes
const long long kTokenGapDefaultMs = 90000;    // 90 seconds

// One watchdog per request. The read loop updates lastTokenMs on a data
// frame; the watchdog kills curl when the gap exceeds the deadline.
struct StallWatch {
    std::atomic<bool> stop{false};
    std::atomic<bool> fired{false};
    std::atomic<bool> sawToken{false};
    std::atomic<long long> lastTokenMs{0};
};

long long monotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

// Rough token count used only when a provider reports no usage. Four bytes
// per token is a conventional English estimate; it is deliberately a
// fallback, not the model's real billing count.
long long estimateTokens(const std::string& text) {
    return static_cast<long long>((text.size() + 3) / 4);
}

long long estimatePromptTokens(const std::vector<Message>& history) {
    size_t bytes = 0;
    for (const Message& message : history)
        bytes += message.role.size() + message.content.size() + 8;
    return static_cast<long long>((bytes + 3) / 4);
}

struct SpawnedCurl {
    pid_t pid = -1;
    int readFd = -1;
};

void registerCompletion(pid_t pid) {
    std::lock_guard<std::mutex> lock(g_completionMutex);
    g_completionPid = pid;
    g_completionActive.store(true);
    g_completionLastActivityMs.store(monotonicMs());
    // A cancel that arrived between the spawn and this registration must
    // still stop the child.
    if (g_completionCancelled) ::kill(pid, SIGKILL);
}

void clearCompletion() {
    std::lock_guard<std::mutex> lock(g_completionMutex);
    g_completionPid = -1;
    g_completionActive.store(false);
}

bool completionCancelled() {
    std::lock_guard<std::mutex> lock(g_completionMutex);
    return g_completionCancelled;
}

bool spawnCurl(const std::string& url, const std::string& apiKey,
               const std::string& bodyPath, const std::string& errPath,
               SpawnedCurl& child, std::string& error) {
    int fds[2];
    if (::pipe(fds) != 0) {
        error = "cannot create a pipe for curl";
        return false;
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        error = "cannot prepare the curl process";
        return false;
    }
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    if (fds[1] != STDOUT_FILENO)
        posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errPath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);

    const std::string auth = "Authorization: Bearer " + apiKey;
    const std::string data = "@" + bodyPath;
    char* argv[] = {
        const_cast<char*>("curl"),
        const_cast<char*>("-sS"),
        const_cast<char*>("-N"),
        const_cast<char*>("--connect-timeout"),
        const_cast<char*>("15"),
        // Abort when the server sends nothing for two minutes, and cap the
        // whole transfer so a trickle cannot park the worker forever.
        const_cast<char*>("--speed-limit"),
        const_cast<char*>("1"),
        const_cast<char*>("--speed-time"),
        const_cast<char*>("120"),
        const_cast<char*>("--max-time"),
        const_cast<char*>("900"),
        const_cast<char*>("-X"),
        const_cast<char*>("POST"),
        const_cast<char*>(url.c_str()),
        const_cast<char*>("-H"),
        const_cast<char*>(auth.c_str()),
        const_cast<char*>("-H"),
        const_cast<char*>("Content-Type: application/json"),
        const_cast<char*>("-d"),
        const_cast<char*>(data.c_str()),
        nullptr};

    pid_t pid = -1;
    const int rc =
        ::posix_spawnp(&pid, "curl", &actions, nullptr, argv, environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        error = std::string("cannot run curl: ") + std::strerror(rc);
        return false;
    }
    child.pid = pid;
    child.readFd = fds[0];
    return true;
}

ChatOutcome runStream(const Config& cfg, const std::vector<Message>& history,
                      const std::string& prefix, FILE* out,
                      bool allowLongTimer, bool quiet) {
    ChatOutcome result;
    g_completionTokensSeen.store(0);
    // Read at call time, not at static init, so a test or an operator can
    // change the deadlines for one run.
    const long long firstTokenMs =
        stallMsFromEnv("AI_CHAT_FIRST_TOKEN_MS", kFirstTokenDefaultMs);
    const long long tokenGapMs =
        stallMsFromEnv("AI_CHAT_TOKEN_GAP_MS", kTokenGapDefaultMs);
    std::ostringstream payload;
    payload << "{\"model\":\"" << jsonEscape(cfg.model, quiet)
            << "\",\"stream\":true,"
               "\"stream_options\":{\"include_usage\":true},\"messages\":[";
    for (size_t i = 0; i < history.size(); ++i) {
        if (i) payload << ',';
        payload << "{\"role\":\"" << history[i].role << "\",\"content\":\""
                << jsonEscape(history[i].content, quiet) << "\"}";
    }
    payload << "]}";

    TempFile body = makeTemp("ai-chat-req", payload.str());
    if (body.path.empty()) {
        result.status = ChatStatus::TransportError;
        result.error = "cannot create temp files in /tmp";
        if (!quiet) std::fprintf(stderr, "ai-chat: %s\n", result.error.c_str());
        return result;
    }

    std::string url = cfg.base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.find("chat/completions") == std::string::npos)
        url += "/chat/completions";

    // Providers sometimes drop the connection before answering (DeepSeek in
    // particular is known to close TLS streams without a close_notify). A
    // silent, byte-less failure is worth retrying; the conversation and the
    // user's message stay intact.
    constexpr int kMaxAttempts = 3;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        TempFile errf = makeTemp("ai-chat-err", "");
        if (errf.path.empty()) {
            result.status = ChatStatus::TransportError;
            result.error = "cannot create temp files in /tmp";
            if (!quiet)
                std::fprintf(stderr, "ai-chat: %s\n", result.error.c_str());
            return result;
        }

        if (completionCancelled()) {
            result.status = ChatStatus::TransportError;
            result.error = "the request was cancelled";
            return result;
        }

        SpawnedCurl child;
        std::string spawnError;
        if (!spawnCurl(url, cfg.api_key, body.path, errf.path, child,
                       spawnError)) {
            result.status = ChatStatus::TransportError;
            result.error = spawnError;
            if (!quiet)
                std::fprintf(stderr, "ai-chat: %s\n", spawnError.c_str());
            return result;
        }
        registerCompletion(child.pid);

        FILE* pipe = ::fdopen(child.readFd, "r");
        if (!pipe) {
            ::close(child.readFd);
            ::kill(child.pid, SIGKILL);
            int ignored = 0;
            ::waitpid(child.pid, &ignored, 0);
            clearCompletion();
            result.status = ChatStatus::TransportError;
            result.error = "cannot read curl output";
            if (!quiet)
                std::fprintf(stderr, "ai-chat: %s\n", result.error.c_str());
            return result;
        }

        SentencePrinter printer(out, allowLongTimer);
        std::string raw, line;
        char buf[8192];
        bool sawDone = false;
        bool sawData = false;
        bool prefixed = quiet || out == nullptr;
        long long promptTokens = 0;
        long long completionTokens = 0;
        long long totalTokens = 0;

        // Watch model progress, not wire activity. The watchdog only kills the
        // child; fgets() then sees EOF and the normal reaping path runs.
        auto watch = std::make_shared<StallWatch>();
        watch->lastTokenMs.store(monotonicMs());
        std::thread watchdog([watch, pid = child.pid, firstTokenMs,
                              tokenGapMs]() {
            while (!watch->stop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (watch->stop.load()) break;
                if (sse_watch::stalled(monotonicMs(),
                                       watch->lastTokenMs.load(),
                                       watch->sawToken.load(), firstTokenMs,
                                       tokenGapMs)) {
                    watch->fired.store(true);
                    ::kill(pid, SIGKILL);
                    break;
                }
            }
        });

        while (fgets(buf, sizeof buf, pipe)) {
            raw += buf;
            line = trim(buf);
            if (!startsWith(line, "data:")) continue; // comments / headers / errors
            sawData = true;
            if (sse_watch::isTokenFrame(line)) {
                const long long now = monotonicMs();
                watch->lastTokenMs.store(now);
                watch->sawToken.store(true);
                g_completionLastActivityMs.store(now);
            }
            std::string data = trim(line.substr(5));
            if (data == "[DONE]") { sawDone = true; break; }
            extractUsage(data, promptTokens, completionTokens, totalTokens);
            std::string delta;
            if (extractDelta(data, delta) && !delta.empty()) {
                if (!prefixed) {
                    FILE* dst = out ? out : stdout;
                    std::fputs(prefix.c_str(), dst);
                    std::fflush(dst);
                    prefixed = true;
                }
                printer.feed(delta);
                const long long seen = estimateTokens(delta);
                g_completionTokensSeen.fetch_add(seen > 0 ? seen : 1);
            }
        }
        watch->stop.store(true);
        if (watchdog.joinable()) watchdog.join();
        // Close the read end before reaping: a child still writing after
        // [DONE] then gets SIGPIPE instead of blocking us in waitpid().
        ::fclose(pipe);
        int status = 0;
        ::waitpid(child.pid, &status, 0);
        clearCompletion();
        int rc = exitCodeOf(status);
        printer.finish();
        if (totalTokens <= 0) {
            promptTokens = estimatePromptTokens(history);
            completionTokens = estimateTokens(printer.text());
            totalTokens = promptTokens + completionTokens;
        }

        if (completionCancelled()) {
            result.status = ChatStatus::TransportError;
            result.error = "the request was cancelled";
            result.code = rc;
            return result;
        }

        // A complete stream wins over a late kill: [DONE] means the reply is
        // whole even if the watchdog fired in the same instant.
        if (watch->fired.load() && !sawDone) {
            result.status = ChatStatus::Stalled;
            result.error =
                "the stream stayed open but sent no token for " +
                std::to_string(tokenGapMs / 1000) +
                "s (or no first token within " +
                std::to_string(firstTokenMs / 1000) +
                "s); the request was stopped";
            result.code = rc;
            return result;
        }

        std::string errText = readFile(errf.path);
        std::string snippet = trim(raw);

        // A reply can consist only of a protocol tag (for example a model
        // that just says <sca/>). That still counts as an answer.
        const bool answered = !printer.text().empty() || printer.wantsWait() ||
                              printer.wantsCognition() ||
                              printer.wantsAutonomy() || printer.timerSec() > 0 ||
                              !printer.trackRequests().empty() ||
                              !printer.lessonRequests().empty() ||
                              !printer.storyRequests().empty();
        if (answered) {
            result.text = printer.text();
            result.wantsWait = printer.wantsWait();
            result.wantsCognition = printer.wantsCognition();
            result.wantsAutonomy = printer.wantsAutonomy();
            result.timerSec = printer.timerSec();
            result.trackRequests = printer.trackRequests();
            result.lessonRequests = printer.lessonRequests();
            result.storyRequests = printer.storyRequests();
            result.promptTokens = promptTokens;
            result.completionTokens = completionTokens;
            result.totalTokens = totalTokens;
            result.code = rc;
            result.status = ChatStatus::Ok;
            if (!sawDone && !quiet)
                std::printf("\n%s(stream ended early)%s\n", ui::dim(), ui::off());
            return result;
        }

        // A complete stream with no text is an empty reply, not an API error.
        if (sawDone) {
            result.status = ChatStatus::EmptyReply;
            result.code = rc;
            result.error = "the model returned no text";
            return result;
        }

        // A non-SSE body is the server rejecting the request. Retrying the
        // same bytes would just get the same answer.
        if (!sawData && !snippet.empty()) {
            result.status = ChatStatus::ApiError;
            result.code = rc;
            result.error = text_utf8::head(text_utf8::sanitize(snippet), 2000);
            if (!quiet) {
                std::fprintf(stderr,
                             "\n%s! the request did not produce a reply\n",
                             ui::red());
                if (!errText.empty())
                    std::fprintf(stderr, "  %s\n", errText.c_str());
                snippet = text_utf8::head(snippet, 600) + "...";
                std::fprintf(stderr, "  server said: %s\n", snippet.c_str());
                std::fprintf(stderr, "%s", ui::off());
            }
            return result;
        }

        bool transient = rc == 0 || transientCurlError(rc);
        if (transient && attempt < kMaxAttempts) {
            int delaySec = attempt; // 1s, then 2s
            if (!quiet) {
                if (rc == 0)
                    std::fprintf(
                        stderr,
                        "%s! the API returned an empty response, retrying "
                        "in %ds… (%d/%d)%s\n",
                        ui::yellow(), delaySec, attempt, kMaxAttempts,
                        ui::off());
                else
                    std::fprintf(
                        stderr,
                        "%s! no reply from the API (curl exit %d), "
                        "retrying in %ds… (%d/%d)%s\n",
                        ui::yellow(), rc, delaySec, attempt, kMaxAttempts,
                        ui::off());
                std::fflush(stderr);
            }
            ::sleep(static_cast<unsigned>(delaySec));
            continue;
        }

        result.code = rc;
        if (rc != 0) {
            result.status = ChatStatus::TransportError;
            result.error = "the connection was closed before the API answered "
                           "(curl exit " +
                           std::to_string(rc) + ")";
        } else {
            result.status = ChatStatus::StreamError;
            result.error = "the API returned an empty response";
        }

        if (!quiet) {
            std::fprintf(stderr, "\n%s! the request did not produce a reply\n",
                         ui::red());
            if (rc != 0)
                std::fprintf(stderr,
                             "  the connection was closed before the API "
                             "answered (curl exit %d)\n",
                             rc);
            else
                std::fprintf(stderr,
                             "  the API returned an empty response — check "
                             "base_url / model / API key\n");
            if (!errText.empty())
                std::fprintf(stderr, "  %s\n", errText.c_str());
            std::fprintf(stderr, "%s", ui::off());
        }
        return result;
    }

    // Unreachable: every attempt returns from inside the loop.
    result.status = ChatStatus::TransportError;
    result.error = "no attempt was made";
    return result;
}

} // namespace

void cancelCompletion() {
    std::lock_guard<std::mutex> lock(g_completionMutex);
    g_completionCancelled = true;
    // The pid is only cleared after waitpid(), so it cannot have been reused.
    if (g_completionPid > 0) ::kill(g_completionPid, SIGKILL);
}

void resetCompletion() {
    std::lock_guard<std::mutex> lock(g_completionMutex);
    g_completionCancelled = false;
}

bool completionActive() { return g_completionActive.load(); }

long long completionLastActivityMs() {
    return g_completionLastActivityMs.load();
}

long long completionTokensSeen() {
    return g_completionTokensSeen.load();
}

const char* chatStatusName(ChatStatus status) {
    switch (status) {
    case ChatStatus::Ok: return "ok";
    case ChatStatus::EmptyReply: return "empty reply";
    case ChatStatus::ApiError: return "API error";
    case ChatStatus::TransportError: return "transport error";
    case ChatStatus::StreamError: return "stream error";
    case ChatStatus::Stalled: return "stalled stream";
    }
    return "unknown";
}

bool streamChat(const Config& cfg, const std::vector<Message>& history,
                const std::string& prefix, std::string& reply,
                bool& wantsWait, bool& wantsCognition, bool& wantsAutonomy,
                int& timerSec, bool allowLongTimer,
                std::vector<TrackRequest>& trackRequests,
                std::vector<std::string>& lessonRequests,
                std::vector<StoryRequest>& storyRequests) {
    // A cancel only applies to the request that was running when it arrived.
    resetCompletion();
    const ChatOutcome outcome =
        runStream(cfg, history, prefix, stdout, allowLongTimer, false);
    reply = outcome.text;
    wantsWait = outcome.wantsWait;
    wantsCognition = outcome.wantsCognition;
    wantsAutonomy = outcome.wantsAutonomy;
    timerSec = outcome.timerSec;
    trackRequests = outcome.trackRequests;
    lessonRequests = outcome.lessonRequests;
    storyRequests = outcome.storyRequests;
    return outcome.ok();
}

ChatOutcome completeChatOutcome(const Config& cfg,
                                const std::vector<Message>& history) {
    return runStream(cfg, history, "", nullptr, false, true);
}

bool completeChat(const Config& cfg, const std::vector<Message>& history,
                  std::string& reply) {
    const ChatOutcome outcome = completeChatOutcome(cfg, history);
    reply = outcome.text;
    return outcome.ok();
}
