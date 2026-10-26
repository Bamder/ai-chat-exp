#pragma once

#include <string>

struct Config {
    std::string api_key;
    std::string base_url = "https://api.openai.com/v1";
    std::string model    = "gpt-4o-mini";
    std::string nick     = "ai"; // what the AI is called
    int         timeout  = 30;   // seconds the user gets per turn
    std::string login_id;        // last session login-id (not a secret, not
                                 // an identity-tracker object)
};

// Cleans a login-id typed by the user. Login-ids are labels, not
// authentication: letters, digits, '.', '_', '-' and '@' are accepted, plus
// non-ASCII bytes so a name in any language can be used. The result is at most
// 64 bytes. Returns an empty string when the input is not acceptable.
std::string normalizeLoginId(const std::string& raw);

// The login-id offered as the default at startup: $USER when it is usable,
// otherwise "user".
std::string defaultLoginId();

// Base directory for the AI's state. This replaces HOME: AI_CHAT_HOME is the
// anchor for the config, memory, workspace and self-made tools.
std::string aiChatHome();
// Exports AI_CHAT_HOME for this process (and the tools it spawns).
void setAiChatHome(const std::string& path);

// Directory that holds the config and the AI's durable memory files:
// $AI_CHAT_HOME/.config/ai-chat.
std::string configDirPath();
// Joins a file or directory name onto configDirPath(). Empty when
// AI_CHAT_HOME is unset.
std::string configSubpath(const std::string& name);
// Creates the directory (mode 0700) if it does not exist yet.
void ensureConfigDir();

// Creates $path and $path/.config/ai-chat (mode 0700) and verifies that the
// state directory is writable. Returns false and fills `error` on failure,
// including missing permissions.
bool ensureAiChatHome(const std::string& path, std::string& error);

std::string configPath();
Config loadConfig();
bool saveConfig(const Config& c);
