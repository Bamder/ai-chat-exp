#pragma once

#include <string>
#include <vector>

#include "aichat/core/config.h"

// The model appends this tag when it wants to wait for the user.
inline constexpr const char* WAIT_TAG = "<wait/>";

// The model appends this tag after telling the user that it wants to start an
// autonomous background loop.
inline constexpr const char* AUTO_TAG = "<auto/>";

// The model may choose the next countdown with a tag like <timer 90/>.
// When it picks a timer on its own initiative, values outside this range are
// clamped. If the user explicitly gave an exact time, a longer timer is
// allowed, up to TIMER_HARD_MAX_SEC.
inline constexpr int TIMER_MIN_SEC = 30;
inline constexpr int TIMER_MAX_SEC = 180;
inline constexpr int TIMER_HARD_MAX_SEC = 24 * 60 * 60;

struct Message {
    std::string role; // "system" | "user" | "assistant"
    std::string content;
};

// Why a completion did or did not produce text. Callers must not collapse
// these into one "no reply" case: a transport failure is not a model decision,
// and an empty reply is not a delivery failure.
enum class ChatStatus {
    Ok,             // the call succeeded; `text` may still be empty for a
                    // protocol-tag-only reply (check wants* flags)
    EmptyReply,     // 200 with no text and no tags
    ApiError,       // the server rejected the request (4xx/5xx body)
    TransportError, // curl/DNS/TLS/timeout; the request may not have arrived
    StreamError,    // 200 but no parseable deltas
    Stalled,        // the stream stayed alive but sent no data frame for too
                    // long; the request was stopped on purpose
};

const char* chatStatusName(ChatStatus status);

// A request emitted by the model as <track id="..." kind="..." text="..."/>.
// The app validates it and writes it to the identity tracker; the model never
// touches the file.
struct TrackRequest {
    std::string id;
    std::string kind;
    std::string text;
};

// A request emitted by the model as <story object="...">...</story>. The app
// writes the narrative; the model never touches the story file.
struct StoryRequest {
    std::string object; // empty for the life story
    std::string body;
};

struct ChatOutcome {
    ChatStatus status = ChatStatus::TransportError;
    std::string text;   // protocol tags stripped
    std::string error;  // server body or transport message, for the journal
    int code = 0;       // curl exit code when available
    bool wantsWait = false;
    bool wantsCognition = false;
    bool wantsAutonomy = false;
    int timerSec = 0;
    std::vector<TrackRequest> trackRequests;
    std::vector<std::string> lessonRequests;
    std::vector<StoryRequest> storyRequests;
    // Token usage reported by the API. When the provider does not send usage
    // chunks, the app fills these with a rough byte-based estimate for a
    // successful reply.
    long long promptTokens = 0;
    long long completionTokens = 0;
    long long totalTokens = 0;

    bool ok() const { return status == ChatStatus::Ok; }
};

// Streams one assistant reply. The first line of the reply is prefixed
// with `prefix` (e.g. "Mochi> "). Returns false on a transport/API failure.
// `timerSec` is 0 when the reply did not pick a countdown. When
// `allowLongTimer` is true (the user gave an exact time), timer values above
// TIMER_MAX_SEC are accepted instead of clamped. `wantsCognition` is set when
// the reply ends with <sca/>, asking the app to rewrite the self-cognition
// file. `wantsAutonomy` is set when it ends with <auto/>, asking the app to
// start the autonomous background loop. `trackRequests` receives the
// <track .../> requests found in the reply, removed from `reply`.
// `lessonRequests` receives the <lesson>...</lesson> bodies found in the
// reply, also removed from `reply`. `storyRequests` receives the
// <story object="...">...</story> blocks, likewise removed.
bool streamChat(const Config& cfg, const std::vector<Message>& history,
                const std::string& prefix, std::string& reply,
                bool& wantsWait, bool& wantsCognition, bool& wantsAutonomy,
                int& timerSec, bool allowLongTimer,
                std::vector<TrackRequest>& trackRequests,
                std::vector<std::string>& lessonRequests,
                std::vector<StoryRequest>& storyRequests);

// Runs a background completion without printing anything to stdout. The
// result carries the delivery status, the server/transport error, and the
// reply with protocol tags removed.
ChatOutcome completeChatOutcome(const Config& cfg,
                                const std::vector<Message>& history);

// Stops the completion that is currently in flight, if any. Safe to call from
// another thread: the blocked completion returns a transport error instead of
// waiting for a silent server. The cancelled state is latched until
// resetCompletion(), so an in-progress shutdown cannot start a new request.
void cancelCompletion();
void resetCompletion();

// Live probe for the background-worker UI. `completionActive` is true while a
// request is in flight; `completionLastActivityMs` is the monotonic time of
// the last chunk read from it. The pair lets the prompt keep its spinner
// moving while data arrives and stop it when the server goes silent.
bool completionActive();
long long completionLastActivityMs();
// Live estimate of the completion tokens received so far for the request in
// flight. It grows as text streams and resets when a new request starts.
long long completionTokensSeen();

// Compatibility wrapper. Returns true only for ChatStatus::Ok; prefer
// completeChatOutcome when the caller needs to distinguish failure modes.
bool completeChat(const Config& cfg, const std::vector<Message>& history,
                  std::string& reply);
