#pragma once

#include <string>
#include <vector>

#include "aichat/core/config.h"

namespace ui {

void setColorEnabled(bool on);

// Test mode is a session-only, read-only mode: the self-cognition, identity
// tracker, and documentary memory files are never written and long
// conversations are not compacted. The prompt turns yellow and reads
// "You (login-id, Test Mode)".
void setTestMode(bool on);
bool testMode();

// The login-id for this session. It is shown at the input prompt and in the
// banner, and it is passed to the model. It is not an identity-tracker object.
void setLoginId(const std::string& loginId);
std::string loginId();

const char* dim();
const char* bold();
const char* cyan();
const char* green();
const char* yellow();
const char* red();
const char* orange();
const char* purple();
const char* softBlue();
const char* gray();
const char* brightGray();
const char* off();

void usage();
void banner(const Config& c);
void help();

// The "You > " label used at the input prompt and in the transcript.
std::string userPrompt();
// Prints a user message to the transcript, prefixed with userPrompt().
void echoUser(const std::string& text);
// Prints one dim status line ("* ...").
void note(const std::string& text);

// One colored piece of a status line.
struct TextPart {
    std::string text;
    const char* color;
};

// Prints a status line made of colored parts. With an empty `indent` the line
// gets the dim "* " bullet; otherwise `indent` replaces the bullet (used for
// continuation lines).
void noteParts(const std::vector<TextPart>& parts,
               const std::string& indent = "");
// Builds the system prompt. `cognition` is the current self-cognition file,
// `tracker` the durable identity tracker (objects, aliases, roles and facts),
// and `documentary` the rendered long-term memory of earlier periods; all may
// be empty. `login` is the current login-id (a session label, not a tracker
// object) and is never empty in a normal run.
std::string systemPrompt(int timeout, const std::string& nick,
                         const std::string& cognition = "",
                         const std::string& documentary = "",
                         const std::string& autonomy = "",
                         const std::string& manuals = "",
                         const std::string& tracker = "",
                         const std::string& login = "",
                         const std::string& practice = "",
                         const std::string& story = "");
// Makes sure AI_CHAT_HOME is set and its state directory is writable. When the
// variable is missing, asks for it on a terminal, creates it (mode 0700) and
// prints the export line; re-asks when permissions are missing. Returns false
// when the user aborts, input is not a terminal, or creation keeps failing.
bool setupAiChatHome();
bool setupWizard(Config& c);
// Ensures a login-id is present and, when `ask` is true, prompts for it with
// the saved/default one as the default. A new login-id typed at the prompt is
// saved to the config (unless test mode is on). Returns false on EOF.
bool setupLogin(Config& c, bool ask);

} // namespace ui
