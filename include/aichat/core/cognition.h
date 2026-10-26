#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "aichat/core/chat.h"

// The model appends this tag when it feels its self-cognition file needs to
// be rewritten. Like <wait/> and <timer N/> it is stripped from the reply.
inline constexpr const char* SCA_TAG = "<sca/>";

// Above this many characters of context the app compacts the conversation
// and refreshes the self-cognition file at the same time. It is a rough
// proxy for tokens (about 8k tokens), deliberately well below the context
// window of current models.
inline constexpr std::size_t CONTEXT_SOFT_LIMIT = 32000;

// Durable memory of the AI. The questions ship with the source, the answers
// live in the config directory:
//
//   manuals/self-cognition.md  the questions (also: AI_CHAT_QUESTIONS)
//   self-cognition.sca         the answers (always the latest version only)
//
// The .sca file is overwritten on every refresh. Before a new one is
// written the previous one is passed to the model, so the AI evolves its
// self-description instead of starting from scratch.
namespace cognition {

std::string questionsPath();
std::string filePath();

// The questions document. Read from manuals/self-cognition.md, resolved
// relative to the working directory and then the executable, so edits take
// effect on the next refresh. Falls back to an optional copy in the config
// directory, then to the built-in questions.
std::string questions();

// Reads the latest .sca file (empty when there is none).
std::string load();

// Asks the model to update the file using the previous version and a short
// account of what happened recently (`recent`). `tracker` is the rendered
// identity tracker, authoritative for objects and facts; it may be empty. On
// success writes the new version atomically and returns its full text in
// `saved`. `quiet` suppresses the failure diagnostic on stderr; background
// callers use it because the chat prompt owns the terminal.
bool refresh(const Config& cfg, const std::string& previous,
             const std::string& recent, const std::string& tracker,
             std::string& saved, bool quiet = false);

} // namespace cognition
