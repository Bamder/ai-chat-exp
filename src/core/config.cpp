#include "aichat/core/config.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aichat/core/util.h"

namespace {

// Creates every missing component of `dir` with mode 0700 and checks that the
// final directory is writable. Unlike a plain mkdir loop, failures are
// reported instead of ignored: a deploy without permissions must not look
// like a successful setup.
bool ensureDir(const std::string& dir, std::string& error) {
    if (dir.empty()) {
        error = "the directory path is empty";
        return false;
    }
    std::string acc;
    std::stringstream ss(dir);
    std::string part;
    bool absolute = dir[0] == '/';
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        if (acc.empty() && !absolute) acc = part;
        else acc += "/" + part;
        if (::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) {
            error = "cannot create " + acc + ": " + std::strerror(errno);
            return false;
        }
    }

    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        error = "cannot inspect " + dir + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        error = dir + " is not a directory";
        return false;
    }
    if (::access(dir.c_str(), W_OK | X_OK) != 0) {
        error = "directory is not writable: " + dir + ": " +
                std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

std::string aiChatHome() {
    const char* home = getenv("AI_CHAT_HOME");
    return home && *home ? std::string(home) : std::string();
}

void setAiChatHome(const std::string& path) {
    std::string clean = path;
    while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
    ::setenv("AI_CHAT_HOME", clean.c_str(), 1);
}

std::string configDirPath() {
    const std::string home = aiChatHome();
    return home.empty() ? std::string() : home + "/.config/ai-chat";
}

std::string configSubpath(const std::string& name) {
    const std::string dir = configDirPath();
    return dir.empty() ? std::string() : dir + "/" + name;
}

void ensureConfigDir() {
    std::string error;
    ensureDir(configDirPath(), error);
}

bool ensureAiChatHome(const std::string& path, std::string& error) {
    if (path.empty()) {
        error = "AI_CHAT_HOME is empty";
        return false;
    }
    if (path[0] != '/') {
        error = "AI_CHAT_HOME must be an absolute path: " + path;
        return false;
    }

    std::string base = path;
    while (base.size() > 1 && base.back() == '/') base.pop_back();

    if (!ensureDir(base, error)) return false;
    return ensureDir(base + "/.config/ai-chat", error);
}

std::string configPath() {
    return configSubpath("config");
}

std::string normalizeLoginId(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty() || s.size() > 64) return std::string();
    for (unsigned char c : s) {
        if (c >= 0x80) continue;
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != '@')
            return std::string();
    }
    return s;
}

std::string defaultLoginId() {
    const char* user = getenv("USER");
    const std::string clean = user ? normalizeLoginId(user) : std::string();
    return clean.empty() ? std::string("user") : clean;
}

Config loadConfig() {
    Config c;
    std::ifstream in(configPath());
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1)); // keys may contain '='
        if      (k == "api_key")  c.api_key  = v;
        else if (k == "base_url") { if (!v.empty()) c.base_url = v; }
        else if (k == "model")    { if (!v.empty()) c.model    = v; }
        else if (k == "nick")     { if (!v.empty()) c.nick     = v; }
        else if (k == "timeout")  { int t = atoi(v.c_str()); if (t >= 0) c.timeout = t; }
        else if (k == "login_id") { c.login_id = normalizeLoginId(v); }
    }
    return c;
}

bool saveConfig(const Config& c) {
    const std::string dir = configDirPath();
    const std::string file = configPath();
    if (dir.empty() || file.empty()) return false;

    std::string error;
    if (!ensureDir(dir, error)) return false;

    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << "api_key="  << c.api_key  << "\n"
        << "base_url=" << c.base_url << "\n"
        << "model="    << c.model    << "\n"
        << "nick="     << c.nick     << "\n"
        << "timeout="  << c.timeout  << "\n";
    if (!c.login_id.empty())
        out << "login_id=" << c.login_id << "\n";
    out.close();
    if (!out) return false;
    return ::chmod(file.c_str(), 0600) == 0; // the key is a secret
}
