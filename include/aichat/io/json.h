#pragma once

#include <string>

// Escapes `s` for a JSON string, replacing invalid UTF-8. Unless `quiet` is
// set, a warning is printed when a repair was needed; background callers set
// it because the chat prompt owns the terminal.
std::string jsonEscape(const std::string& s, bool quiet = false);
std::string jsonUnescape(const std::string& s);

// Extract the streamed text delta from a chat.completion.chunk payload.
bool extractDelta(const std::string& json, std::string& out);

// Extract reported token usage from a chat completion chunk. Returns false
// when the chunk carries no usage object with numeric counts. Missing totals
// are filled from the prompt and completion counts.
bool extractUsage(const std::string& json, long long& promptTokens,
                  long long& completionTokens, long long& totalTokens);
