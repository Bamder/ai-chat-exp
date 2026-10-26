#pragma once

#include <string>

// The sse-watch tool separates transport liveness from model progress in a
// Server-Sent Events stream. A `:` comment or an HTTP header proves the
// connection is alive; only a `data:` frame carrying a non-empty delta proves
// the model is producing. A watchdog that counts bytes cannot tell a stalled
// generation from a chatty keep-alive, which is how a merge can sit silent for
// many minutes without looking dead.
namespace sse_watch {

// True when `line` is an SSE data frame that carries model output: it starts
// with "data:", is not "[DONE]", and has a non-empty "content" or
// "reasoning_content" string. Comments, headers, role-only frames, empty
// deltas and heartbeats return false.
bool isTokenFrame(const std::string& line);

// True when the stream has gone too long without a token. `lastMs` is the
// time of the last token, or the request start when `sawToken` is false.
// A non-positive limit disables that check. Comparison is strict (`>`), so a
// limit of 90 s fires on the first check after 90 s, not at exactly 90 s.
bool stalled(long long nowMs, long long lastMs, bool sawToken,
             long long firstTokenMs, long long gapMs);

} // namespace sse_watch
