#include "ai-tools/local-dir-access/local-dir-access.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <climits>
#include <sys/stat.h>
#include <unistd.h>

#include "aichat/core/facts/fact.h"
#include "aichat/core/util.h"

namespace {

struct State {
    std::mutex mutex;
    std::condition_variable cv;

    bool pending = false;
    std::string pendingPath;
    unsigned long long pendingId = 0;
    unsigned long long nextId = 1;

    bool hasDecision = false;
    bool allow = false;
    std::string reason;

    std::vector<std::string> roots;
};

State g;

std::string expandHome(const std::string& path) {
    if (path == "~") {
        const char* home = std::getenv("AI_CHAT_HOME");
        return home && *home ? std::string(home) : path;
    }
    if (path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("AI_CHAT_HOME");
        if (home && *home) return std::string(home) + path.substr(1);
    }
    return path;
}

bool inside(const std::string& root, const std::string& path) {
    if (path == root) return true;
    if (path.size() <= root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    return path[root.size()] == '/' || root == "/";
}

std::string canonicalDir(const std::string& raw) {
    std::string path = expandHome(trim(raw));
    if (path.empty()) return "";
    char resolved[PATH_MAX];
    if (!::realpath(path.c_str(), resolved)) return "";
    struct stat st;
    if (::stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return "";
    return resolved;
}

std::string pathArg(const std::map<std::string, std::string>& args) {
    auto it = args.find("path");
    return it == args.end() ? std::string() : trim(it->second);
}

std::string failure(const std::string& message) { return "error: " + message; }

class LocalDirAccessAction : public action::Action {
  public:
    const char* name() const override { return "local_dir_access"; }

    const char* usage() const override {
        return "local_dir_access(path) — ask the user for read access to a "
               "local directory outside the workspace or launch directory; "
               "the approval lasts for this session and the directory is "
               "listed on success";
    }

    action::Result run(
        const std::map<std::string, std::string>& args) const override {
        action::Result result;
        const std::string path = pathArg(args);
        if (path.empty()) {
            result.output = failure("local_dir_access needs a path");
            return result;
        }

        std::string resolved;
        std::string error;
        if (!local_dir_access::request(path, resolved, error)) {
            result.output = failure(error);
            return result;
        }

        // The grant is already active, so list_dir can read the directory.
        // This gives the model immediate useful output instead of making it
        // spend another step on a listing.
        action::Request listing;
        listing.name = "list_dir";
        listing.args["path"] = resolved;
        listing.args["limit"] = "80";
        action::Result listed = action::execute(listing);

        result.ok = true;
        result.output = "permission granted for this session: " + resolved;
        if (!listed.output.empty()) result.output += "\n" + listed.output;
        result.facts.push_back(facts::make(
            facts::FactKind::Grant, "grant:" + resolved, resolved,
            "read access active", facts::FactStatus::Verified, "action"));
        return result;
    }
};

LocalDirAccessAction g_action;

} // namespace

namespace local_dir_access {

const action::Action* tool() { return &g_action; }

bool request(const std::string& raw, std::string& resolved,
             std::string& error) {
    resolved.clear();
    error.clear();

    const std::string path = canonicalDir(raw);
    if (path.empty()) {
        error = "not an existing directory: " + trim(raw);
        return false;
    }

    std::unique_lock<std::mutex> lock(g.mutex);
    for (const std::string& root : g.roots) {
        if (inside(root, path)) {
            resolved = path;
            return true;
        }
    }
    if (g.pending) {
        error = "another directory-access request is already pending";
        return false;
    }
    if (!::isatty(STDIN_FILENO)) {
        error = "read access to " + path +
                " needs interactive approval from the user";
        return false;
    }

    const unsigned long long id = g.nextId++;
    g.pending = true;
    g.pendingPath = path;
    g.pendingId = id;
    g.hasDecision = false;
    g.allow = false;
    g.reason.clear();
    g.cv.notify_all();

    // Wait for the human decision. There is deliberately no timeout: an
    // unanswered gate parks the loop instead of failing the step, so the
    // worker must not wake up and retry on its own.
    g.cv.wait(lock, [&] { return !g.pending; });

    const bool allowed = g.hasDecision && g.allow;
    const std::string reason = g.reason;
    g.hasDecision = false;
    g.reason.clear();
    g.pendingPath.clear();
    lock.unlock();

    if (!allowed) {
        error = reason.empty() ? "the user denied access to " + path : reason;
        return false;
    }

    lock.lock();
    if (std::find(g.roots.begin(), g.roots.end(), path) == g.roots.end())
        g.roots.push_back(path);
    lock.unlock();

    resolved = path;
    return true;
}

Pending pending() {
    std::lock_guard<std::mutex> lock(g.mutex);
    Pending out;
    out.active = g.pending;
    if (out.active) {
        out.path = g.pendingPath;
        out.id = g.pendingId;
    }
    return out;
}

bool hasPending() {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.pending;
}

bool decide(bool allow) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.pending) return false;
    g.pending = false;
    g.hasDecision = true;
    g.allow = allow;
    g.reason.clear();
    g.cv.notify_all();
    return true;
}

void cancel(const std::string& reason) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.pending) return;
    g.pending = false;
    g.hasDecision = true;
    g.allow = false;
    g.reason = reason.empty() ? "the directory-access request was cancelled"
                              : reason;
    g.cv.notify_all();
}

std::string grantedRoot(const std::string& canonicalPath) {
    std::lock_guard<std::mutex> lock(g.mutex);
    std::string best;
    for (const std::string& root : g.roots) {
        if (inside(root, canonicalPath) && root.size() > best.size())
            best = root;
    }
    return best;
}

} // namespace local_dir_access
