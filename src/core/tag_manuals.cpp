#include "aichat/core/tag_manuals.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <climits>
#include <unistd.h>

#include "aichat/core/config.h"

namespace {

const char* const kTopFile = "instruction-manual-for-top-tags.md";

const char* const kTagFiles[] = {
    "instruction-manual-for-wait.md",
    "instruction-manual-for-sca.md",
    "instruction-manual-for-auto.md",
    "instruction-manual-for-timer.md",
    "instruction-manual-for-track.md",
    "instruction-manual-for-lesson.md",
    "instruction-manual-for-story.md",
};

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool hasTopFile(const std::string& dir) {
    std::ifstream in(dir + "/" + kTopFile);
    return static_cast<bool>(in);
}

std::string executableDir() {
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string path(buf);
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return "";
    return path.substr(0, slash);
}

std::string manualsDirectory() {
    const char* env = std::getenv("AI_CHAT_MANUALS_DIR");
    if (env && *env && hasTopFile(env)) return env;

    std::vector<std::string> candidates;
    candidates.push_back("manuals/tags");
    candidates.push_back("../manuals/tags");

    const std::string exe = executableDir();
    if (!exe.empty()) {
        candidates.push_back(exe + "/manuals/tags");
        candidates.push_back(exe + "/../manuals/tags");
    }

    const std::string stateHome = aiChatHome();
    if (!stateHome.empty())
        candidates.push_back(stateHome + "/.config/ai-chat/manuals/tags");

    for (const std::string& dir : candidates) {
        if (hasTopFile(dir)) return dir;
    }
    return "";
}

std::string join(const std::string& dir, const char* file) {
    std::string text = readFile(dir + "/" + file);
    if (text.empty()) return "";
    std::ostringstream out;
    out << "### " << file << "\n\n" << text;
    if (text.back() != '\n') out << "\n";
    return out.str();
}

} // namespace

namespace tag_manuals {

std::string all() {
    const std::string dir = manualsDirectory();
    if (dir.empty()) return "";

    std::ostringstream out;
    out << join(dir, kTopFile);
    for (const char* file : kTagFiles) {
        const std::string part = join(dir, file);
        if (!part.empty()) out << "\n---\n\n" << part;
    }
    return out.str();
}

} // namespace tag_manuals
