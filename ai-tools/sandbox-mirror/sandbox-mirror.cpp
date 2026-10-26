#include "ai-tools/sandbox-mirror/sandbox-mirror.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/facts/fact.h"
#include "aichat/core/util.h"

namespace {

struct MirrorEntry {
    bool directory = false;
    std::string origin;
    std::string mirror;
};

struct State {
    std::mutex mutex;
    std::vector<MirrorEntry> entries;
};

State g;

struct PermissionState {
    std::mutex mutex;
    std::condition_variable cv;

    bool pending = false;
    bool directory = false;
    std::string origin;
    std::string mirror;
    std::string replaceDestination;
    std::string newDestination;
    std::string explanation;
    unsigned long long pendingId = 0;
    unsigned long long nextId = 1;

    bool hasDecision = false;
    sandbox_mirror::ApplyChoice choice = sandbox_mirror::ApplyChoice::Deny;
    std::string reason;
};

PermissionState p;

std::atomic<unsigned long long> g_tempId{0};

bool inside(const std::string& root, const std::string& path) {
    if (path == root) return true;
    if (path.size() <= root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    return path[root.size()] == '/' || root == "/";
}

std::string normalizePath(const std::string& path) {
    bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part.empty() || part == ".") {
            // nothing
        } else if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(part);
        } else {
            parts.push_back(part);
        }
        if (j == path.size()) break;
        i = j + 1;
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        if (!out.empty() && out.back() != '/') out += '/';
        out += parts[k];
    }
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

std::string expandHome(const std::string& path) {
    if (path == "~") {
        const char* home = std::getenv("AI_CHAT_HOME");
        return home && *home ? home : path;
    }
    if (path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("AI_CHAT_HOME");
        if (home && *home) return std::string(home) + path.substr(1);
    }
    return path;
}

std::string canonicalPath(const std::string& path) {
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) return "";
    return normalizePath(buf);
}

std::string parentOf(const std::string& path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string baseName(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isRegularFile(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool exists(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

// Creates a directory tree without following symlinks below `root`.
// `root` must already be canonical and exist.
bool ensureSafeDir(const std::string& root, const std::string& path,
                   std::string& error) {
    if (!inside(root, path)) {
        error = "path escapes the workspace: " + path;
        return false;
    }

    std::string acc = root;
    size_t i = root.size();
    while (i <= path.size()) {
        if (i < path.size() && path[i] == '/') {
            ++i;
            continue;
        }
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j == i) break;
        acc += "/" + path.substr(i, j - i);
        struct stat st;
        if (::lstat(acc.c_str(), &st) == 0) {
            if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
                error = "not a real directory: " + acc;
                return false;
            }
        } else if (errno != ENOENT) {
            error = "cannot inspect " + acc;
            return false;
        } else if (::mkdir(acc.c_str(), 0700) != 0) {
            error = "cannot create " + acc;
            return false;
        }
        i = j;
    }
    return true;
}

bool ensurePlainDir(const std::string& dir) {
    if (dir.empty() || isDirectory(dir)) return true;
    std::string acc;
    size_t i = 0;
    if (dir[0] == '/') {
        acc = "/";
        i = 1;
    }
    while (i <= dir.size()) {
        size_t j = dir.find('/', i);
        if (j == std::string::npos) j = dir.size();
        std::string part = dir.substr(i, j - i);
        if (!part.empty()) {
            if (!acc.empty() && acc.back() != '/') acc += '/';
            acc += part;
            if (::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
        if (j == dir.size()) break;
        i = j + 1;
    }
    return isDirectory(dir);
}

bool workspaceRoot(std::string& root, std::string& error) {
    const std::string raw = action::workspacePath();
    if (!ensurePlainDir(raw)) {
        error = "cannot create the workspace directory";
        return false;
    }
    root = canonicalPath(raw);
    if (root.empty()) {
        error = "cannot resolve the workspace directory";
        return false;
    }
    return true;
}

std::string sanitizeName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '.' || c == '-' || c == '_')
            out += c;
        else
            out += '_';
    }
    if (out.empty()) out = "mirror";
    if (out[0] == '.') out = "mirror" + out;
    if (out.size() > 80) out.resize(80);
    return out;
}

std::string uniquePath(const std::string& dir, const std::string& base) {
    std::string candidate = dir + "/" + base;
    for (unsigned long long i = 1; exists(candidate); ++i)
        candidate = dir + "/" + base + "-" + std::to_string(i);
    return candidate;
}

std::string tempPath(const std::string& parent, const char* kind) {
    for (;;) {
        const unsigned long long id = ++g_tempId;
        std::string candidate = parent + "/.ai-chat-" + kind + "-" +
                                std::to_string(::getpid()) + "-" +
                                std::to_string(id);
        if (!exists(candidate)) return candidate;
    }
}

struct CopyStats {
    size_t files = 0;
    size_t dirs = 0;
    size_t skipped = 0;
    size_t bytes = 0;
};

bool copyFile(const std::string& src, const std::string& dst,
              CopyStats& stats, std::string& error) {
    struct stat st;
    if (::lstat(src.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "not a regular file: " + src;
        return false;
    }

    std::ifstream in(src, std::ios::binary);
    if (!in) {
        error = "cannot read " + src;
        return false;
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + dst;
        return false;
    }

    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof buffer);
        const std::streamsize n = in.gcount();
        if (n > 0) out.write(buffer, n);
        if (!out) {
            error = "cannot write " + dst;
            return false;
        }
    }
    if (in.bad()) {
        error = "cannot read " + src;
        return false;
    }
    out.close();
    if (!out) {
        error = "cannot finish writing " + dst;
        return false;
    }
    ::chmod(dst.c_str(), st.st_mode & 0777);
    ++stats.files;
    stats.bytes += static_cast<size_t>(st.st_size);
    return true;
}

// Copies a directory tree. Symlinks and special files are refused when
// `refuseUnsafe` is true, otherwise they are skipped and counted.
bool copyTree(const std::string& src, const std::string& dst,
              bool includeHidden, bool refuseUnsafe, CopyStats& stats,
              std::string& error) {
    struct stat srcStat;
    if (::lstat(src.c_str(), &srcStat) != 0 || !S_ISDIR(srcStat.st_mode)) {
        error = "not a directory: " + src;
        return false;
    }
    if (::mkdir(dst.c_str(), 0700) != 0) {
        error = "cannot create " + dst;
        return false;
    }
    ++stats.dirs;

    DIR* dir = ::opendir(src.c_str());
    if (!dir) {
        error = "cannot read directory " + src;
        return false;
    }

    for (;;) {
        struct dirent* ent = ::readdir(dir);
        if (!ent) break;
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!includeHidden && !name.empty() && name[0] == '.') {
            ++stats.skipped;
            continue;
        }

        const std::string from = src + "/" + name;
        const std::string to = dst + "/" + name;
        struct stat st;
        if (::lstat(from.c_str(), &st) != 0) {
            ++stats.skipped;
            continue;
        }
        if (S_ISLNK(st.st_mode) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
            if (refuseUnsafe) {
                ::closedir(dir);
                error = "refusing to copy a symlink or special file: " + from;
                return false;
            }
            ++stats.skipped;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!copyTree(from, to, includeHidden, refuseUnsafe, stats, error)) {
                ::closedir(dir);
                return false;
            }
        } else if (!copyFile(from, to, stats, error)) {
            ::closedir(dir);
            return false;
        }
    }
    ::closedir(dir);
    return true;
}

int removeEntry(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path) == 0 ? 0 : -1;
}

bool removeTree(const std::string& path, std::string& error) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        error = "cannot inspect " + path;
        return false;
    }
    if (::nftw(path.c_str(), removeEntry, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        error = "could not completely remove " + path;
        return false;
    }
    return true;
}

std::string argText(const std::map<std::string, std::string>& args,
                    const char* name) {
    auto it = args.find(name);
    return it == args.end() ? std::string() : trim(it->second);
}

// Reads the current argument name, falling back to a legacy alias. "path"
// used to mean both the external source and the workspace mirror, which made
// apply/drop easy to get wrong.
std::string argTextOr(const std::map<std::string, std::string>& args,
                      const char* name, const char* legacy) {
    std::string value = argText(args, name);
    if (value.empty()) value = argText(args, legacy);
    return value;
}

std::string failure(const std::string& message) { return "error: " + message; }

bool addEntry(const MirrorEntry& entry) {
    std::lock_guard<std::mutex> lock(g.mutex);
    for (const MirrorEntry& existing : g.entries) {
        if (existing.mirror == entry.mirror) return false;
    }
    g.entries.push_back(entry);
    return true;
}

bool findEntry(const std::string& mirror, MirrorEntry& out) {
    std::lock_guard<std::mutex> lock(g.mutex);
    for (const MirrorEntry& entry : g.entries) {
        if (entry.mirror == mirror) {
            out = entry;
            return true;
        }
    }
    return false;
}

void eraseEntry(const std::string& mirror) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.entries.erase(
        std::remove_if(g.entries.begin(), g.entries.end(),
                       [&](const MirrorEntry& entry) {
                           return entry.mirror == mirror;
                       }),
        g.entries.end());
}

bool requestPermission(bool directory, const std::string& origin,
                       const std::string& mirror,
                       const std::string& replaceDestination,
                       const std::string& newDestination,
                       const std::string& explanation,
                       sandbox_mirror::ApplyChoice& choice,
                       std::string& error) {
    error.clear();
    choice = sandbox_mirror::ApplyChoice::Deny;
    if (!::isatty(STDIN_FILENO)) {
        error = "applying a sandbox mirror needs interactive approval from the "
                "user";
        return false;
    }

    std::unique_lock<std::mutex> lock(p.mutex);
    if (p.pending) {
        error = "another permission request is already pending";
        return false;
    }

    const unsigned long long id = p.nextId++;
    p.pending = true;
    p.directory = directory;
    p.origin = origin;
    p.mirror = mirror;
    p.replaceDestination = replaceDestination;
    p.newDestination = newDestination;
    p.explanation = explanation;
    p.pendingId = id;
    p.hasDecision = false;
    p.choice = sandbox_mirror::ApplyChoice::Deny;
    p.reason.clear();
    p.cv.notify_all();

    // No timeout: an unanswered request parks the loop at this step instead
    // of turning into a failed action that the planner retries.
    p.cv.wait(lock, [&] { return !p.pending; });

    const bool allowed =
        p.hasDecision && p.choice != sandbox_mirror::ApplyChoice::Deny;
    const sandbox_mirror::ApplyChoice selected = p.choice;
    const std::string reason = p.reason;
    p.origin.clear();
    p.mirror.clear();
    p.replaceDestination.clear();
    p.newDestination.clear();
    p.explanation.clear();
    p.pendingId = 0;
    p.hasDecision = false;
    p.choice = sandbox_mirror::ApplyChoice::Deny;
    p.reason.clear();
    lock.unlock();

    if (!allowed) {
        error = reason.empty() ? "the user did not apply the mirror" : reason;
        return false;
    }
    choice = selected;
    return true;
}

std::string resolveMirrorPath(const std::string& raw,
                              const std::string& workspace) {
    std::string path = expandHome(trim(raw));
    if (path.empty()) return "";
    const std::string candidate =
        path[0] == '/' ? normalizePath(path)
                       : normalizePath(workspace + "/" + path);
    const std::string canonical = canonicalPath(candidate);
    if (canonical.empty() || !inside(workspace, canonical) ||
        canonical == workspace)
        return "";
    return canonical;
}

std::string resolveMirrorDestination(
    const std::string& raw, const std::string& workspace,
    const std::string& source, std::string& error) {
    if (!raw.empty()) {
        const std::string expanded = expandHome(raw);
        const std::string candidate =
            expanded[0] == '/' ? normalizePath(expanded)
                               : normalizePath(workspace + "/" + expanded);
        if (!inside(workspace, candidate) || candidate == workspace) {
            error = "the mirror path must be inside the workspace";
            return "";
        }
        if (!ensureSafeDir(workspace, parentOf(candidate), error)) return "";
        if (exists(candidate)) {
            error = "the mirror path already exists: " + candidate;
            return "";
        }
        return candidate;
    }

    const std::string mirrors = workspace + "/mirrors";
    if (!ensureSafeDir(workspace, mirrors, error)) return "";
    return uniquePath(mirrors, sanitizeName(baseName(source)));
}

bool resolveNewDestination(const std::string& origin, const std::string& raw,
                           std::string& target, std::string& error) {
    const std::string expanded = expandHome(trim(raw));
    if (expanded.empty()) {
        error = "a new destination path is needed";
        return false;
    }
    const std::string wanted =
        expanded[0] == '/' ? normalizePath(expanded)
                           : normalizePath(parentOf(origin) + "/" + expanded);
    const std::string parent = parentOf(wanted);
    const std::string canonicalParent = canonicalPath(parent);
    if (canonicalParent.empty()) {
        error = "the new path's parent directory does not exist: " + parent;
        return false;
    }
    target = normalizePath(canonicalParent + "/" + baseName(wanted));
    if (exists(target)) {
        error = "the new path already exists: " + target;
        return false;
    }
    return true;
}

bool proposeNewDestination(const std::string& origin, bool directory,
                           std::string& target, std::string& error) {
    const std::string parent = parentOf(origin);
    const std::string canonicalParent = canonicalPath(parent);
    if (canonicalParent.empty()) {
        error = "the origin's parent directory does not exist";
        return false;
    }

    const std::string base =
        baseName(origin) + (directory ? "-new" : ".new");
    std::string candidate = canonicalParent + "/" + base;
    for (unsigned long long i = 1; exists(candidate) && i < 1000; ++i)
        candidate = canonicalParent + "/" + base + "-" + std::to_string(i);
    if (exists(candidate)) {
        error = "cannot find an unused new path next to the origin";
        return false;
    }
    target = candidate;
    return true;
}

bool applyFile(const std::string& src, const std::string& dst, bool replace,
               CopyStats& stats, std::string& error) {
    if (!isRegularFile(src)) {
        error = "the mirror is not a regular file: " + src;
        return false;
    }
    if (replace && exists(dst)) {
        struct stat st;
        if (::lstat(dst.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            error = "the replacement target is not a regular file: " + dst;
            return false;
        }
    }

    const std::string parent = parentOf(dst);
    const std::string temp = tempPath(parent, "apply");
    if (!copyFile(src, temp, stats, error)) {
        ::unlink(temp.c_str());
        return false;
    }
    if (!replace && exists(dst)) {
        ::unlink(temp.c_str());
        error = "the new path already exists: " + dst;
        return false;
    }
    if (::rename(temp.c_str(), dst.c_str()) != 0) {
        ::unlink(temp.c_str());
        error = "cannot write " + dst + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool applyDirectory(const std::string& src, const std::string& dst,
                    bool replace, CopyStats& stats, std::string& error) {
    const std::string parent = parentOf(dst);
    const std::string temp = tempPath(parent, "apply");
    if (!copyTree(src, temp, true, true, stats, error)) {
        std::string ignored;
        removeTree(temp, ignored);
        return false;
    }

    if (!replace) {
        if (exists(dst)) {
            std::string ignored;
            removeTree(temp, ignored);
            error = "the new path already exists: " + dst;
            return false;
        }
        if (::rename(temp.c_str(), dst.c_str()) != 0) {
            std::string ignored;
            removeTree(temp, ignored);
            error = "cannot write " + dst + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }

    const bool hadDestination = exists(dst);
    std::string backup;
    if (hadDestination) {
        backup = tempPath(parent, "backup");
        if (::rename(dst.c_str(), backup.c_str()) != 0) {
            std::string ignored;
            removeTree(temp, ignored);
            error = "cannot prepare the replacement of " + dst + ": " +
                    std::strerror(errno);
            return false;
        }
    }
    if (::rename(temp.c_str(), dst.c_str()) != 0) {
        if (hadDestination) ::rename(backup.c_str(), dst.c_str());
        std::string ignored;
        removeTree(temp, ignored);
        error = "cannot write " + dst + ": " + std::strerror(errno);
        return false;
    }
    if (hadDestination) {
        std::string ignored;
        removeTree(backup, ignored);
    }
    return true;
}

action::Result mirrorIn(bool directory,
                        const std::map<std::string, std::string>& args) {
    action::Result r;
    const std::string rawSource = argTextOr(args, "source", "path");
    if (rawSource.empty()) {
        r.output =
            failure("the mirror action needs a source path "
                    "(source=<external path>)");
        return r;
    }

    const std::string source = action::resolveReadablePath(rawSource);
    if (source.empty()) {
        std::string problem = action::readablePathProblem(rawSource);
        if (problem.empty()) problem = "source is not readable";
        r.output = failure(problem);
        return r;
    }

    struct stat st;
    if (::lstat(source.c_str(), &st) != 0) {
        r.output = failure("cannot inspect " + source);
        return r;
    }
    if (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) {
        r.output = failure(directory ? "source is not a directory: " + source
                                     : "source is not a regular file: " + source);
        return r;
    }

    std::string workspace;
    std::string error;
    if (!workspaceRoot(workspace, error)) {
        r.output = failure(error);
        return r;
    }
    if (inside(workspace, source)) {
        r.output = failure("source is already inside the sandbox workspace");
        return r;
    }
    if (directory && inside(source, workspace)) {
        r.output = failure(
            "cannot mirror a directory that contains the sandbox workspace");
        return r;
    }

    const std::string mirror = resolveMirrorDestination(
        argText(args, "mirror"), workspace, source, error);
    if (mirror.empty()) {
        r.output = failure(error);
        return r;
    }

    CopyStats stats;
    const bool copied =
        directory
            ? copyTree(source, mirror, false, false, stats, error)
            : copyFile(source, mirror, stats, error);
    if (!copied) {
        std::string ignored;
        removeTree(mirror, ignored);
        r.output = failure(error);
        return r;
    }

    const std::string canonicalMirror = canonicalPath(mirror);
    if (canonicalMirror.empty() ||
        !addEntry({directory, source, canonicalMirror})) {
        r.output = failure("cannot register the mirror");
        return r;
    }

    std::ostringstream out;
    out << "mirrored " << (directory ? "directory" : "file") << "\n"
        << "origin: " << source << "\n"
        << "mirror: " << canonicalMirror << "\n"
        << "files: " << stats.files << ", directories: " << stats.dirs
        << ", skipped: " << stats.skipped << ", bytes: " << stats.bytes;
    r.ok = true;
    r.output = out.str();
    r.facts.push_back(facts::make(
        facts::FactKind::Mirror, "mirror:" + canonicalMirror, canonicalMirror,
        "origin=" + source + "; files: " + std::to_string(stats.files) +
            ", directories: " + std::to_string(stats.dirs) +
            ", skipped: " + std::to_string(stats.skipped) +
            ", bytes: " + std::to_string(stats.bytes),
        facts::FactStatus::Reported, "stat"));
    r.facts.push_back(facts::make(
        directory ? facts::FactKind::FsDir : facts::FactKind::FsFile,
        "path:" + canonicalMirror, canonicalMirror, "mirrored",
        facts::FactStatus::Reported, "stat"));
    return r;
}

action::Result mirrorApply(bool directory,
                           const std::map<std::string, std::string>& args) {
    action::Result r;
    const std::string rawMirror = argTextOr(args, "mirror", "path");
    if (rawMirror.empty()) {
        r.output =
            failure("the mirror action needs a mirror path "
                    "(mirror=<workspace copy>)");
        return r;
    }

    std::string workspace;
    std::string error;
    if (!workspaceRoot(workspace, error)) {
        r.output = failure(error);
        return r;
    }
    const std::string mirror = resolveMirrorPath(rawMirror, workspace);
    if (mirror.empty()) {
        r.output =
            failure("the mirror path is not inside the workspace: " + rawMirror);
        return r;
    }

    MirrorEntry entry;
    if (!findEntry(mirror, entry)) {
        r.output =
            failure("no mirror with that path was created in this session");
        return r;
    }
    if (entry.directory != directory) {
        r.output = failure("this mirror is a " +
                           std::string(entry.directory ? "directory" : "file") +
                           "; use the matching mirror action");
        return r;
    }

    struct stat mirrorStat;
    if (::lstat(mirror.c_str(), &mirrorStat) != 0 ||
        (directory ? !S_ISDIR(mirrorStat.st_mode)
                   : !S_ISREG(mirrorStat.st_mode))) {
        r.output = failure("the mirror no longer matches its type");
        return r;
    }

    const std::string explanation = argText(args, "explanation");
    if (explanation.empty()) {
        r.output = failure("applying a mirror needs an explanation for the user");
        return r;
    }

    // The replace destination is always the original path.
    std::string replaceDestination = entry.origin;
    const std::string canonicalReplaceParent =
        canonicalPath(parentOf(replaceDestination));
    if (canonicalReplaceParent.empty()) {
        r.output = failure("the origin's parent directory does not exist");
        return r;
    }
    replaceDestination =
        normalizePath(canonicalReplaceParent + "/" +
                      baseName(replaceDestination));

    if (exists(replaceDestination)) {
        struct stat st;
        if (::lstat(replaceDestination.c_str(), &st) != 0 ||
            S_ISLNK(st.st_mode)) {
            r.output = failure("refusing to replace a symbolic link: " +
                               replaceDestination);
            return r;
        }
        const bool rightType =
            directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode);
        if (!rightType) {
            r.output = failure("the origin has the wrong type: " +
                               replaceDestination);
            return r;
        }
    }

    const std::string replaceRoot =
        action::approvedMirrorWriteRoot(replaceDestination);
    if (replaceRoot.empty()) {
        r.output = failure(
            "the origin is not in the launch directory or a granted "
            "directory, or has a hidden path component");
        return r;
    }
    if (directory && replaceDestination == replaceRoot) {
        r.output = failure("refusing to replace a root directory");
        return r;
    }

    // The other option is a proposed new path: use the AI's new_path when
    // given, otherwise derive an unused sibling next to the origin.
    std::string newDestination;
    const std::string newPath = argText(args, "new_path");
    if (!newPath.empty()) {
        if (!resolveNewDestination(entry.origin, newPath, newDestination,
                                   error)) {
            r.output = failure(error);
            return r;
        }
    } else if (!proposeNewDestination(entry.origin, directory, newDestination,
                                      error)) {
        r.output = failure(error);
        return r;
    }

    if (action::approvedMirrorWriteRoot(newDestination).empty()) {
        r.output = failure(
            "the proposed new path is not in the launch directory or a "
            "granted directory, or has a hidden path component");
        return r;
    }

    sandbox_mirror::ApplyChoice choice = sandbox_mirror::ApplyChoice::Deny;
    if (!requestPermission(directory, entry.origin, mirror,
                           replaceDestination, newDestination, explanation,
                           choice, error)) {
        r.output = failure(error);
        return r;
    }

    const bool replace = choice == sandbox_mirror::ApplyChoice::Replace;
    const std::string destination =
        replace ? replaceDestination : newDestination;

    CopyStats stats;
    const bool applied = directory
                             ? applyDirectory(mirror, destination, replace,
                                              stats, error)
                             : applyFile(mirror, destination, replace, stats,
                                         error);
    if (!applied) {
        r.output = failure(error);
        return r;
    }

    std::ostringstream out;
    out << "applied mirror " << (replace ? "over" : "as new path") << "\n"
        << "origin: " << entry.origin << "\n"
        << "mirror: " << mirror << "\n"
        << "destination: " << destination << "\n"
        << "files: " << stats.files << ", directories: " << stats.dirs
        << ", bytes: " << stats.bytes;
    r.ok = true;
    r.output = out.str();
    r.facts.push_back(facts::make(
        facts::FactKind::Wrote, "applied:" + destination, destination,
        "applied mirror; files: " + std::to_string(stats.files) +
            ", directories: " + std::to_string(stats.dirs) +
            ", bytes: " + std::to_string(stats.bytes),
        facts::FactStatus::Reported, "stat"));
    return r;
}

action::Result mirrorDrop(bool directory,
                          const std::map<std::string, std::string>& args) {
    action::Result r;
    const std::string rawMirror = argTextOr(args, "mirror", "path");
    if (rawMirror.empty()) {
        r.output =
            failure("the mirror action needs a mirror path "
                    "(mirror=<workspace copy>)");
        return r;
    }

    std::string workspace;
    std::string error;
    if (!workspaceRoot(workspace, error)) {
        r.output = failure(error);
        return r;
    }
    const std::string mirror = resolveMirrorPath(rawMirror, workspace);
    if (mirror.empty()) {
        r.output =
            failure("the mirror path is not inside the workspace: " + rawMirror);
        return r;
    }

    MirrorEntry entry;
    if (!findEntry(mirror, entry)) {
        r.output =
            failure("no mirror with that path was created in this session");
        return r;
    }
    if (entry.directory != directory) {
        r.output = failure("this mirror is a " +
                           std::string(entry.directory ? "directory" : "file") +
                           "; use the matching mirror action");
        return r;
    }

    if (!removeTree(mirror, error)) {
        r.output = failure(error);
        return r;
    }
    eraseEntry(mirror);

    r.ok = true;
    r.output = "dropped mirror: " + mirror;
    r.facts.push_back(facts::make(
        facts::FactKind::Generic, "dropped:" + mirror, mirror, "dropped mirror",
        facts::FactStatus::Reported, "action"));
    return r;
}

class MirrorAction : public action::Action {
  public:
    explicit MirrorAction(bool directory) : directory_(directory) {}

    const char* name() const override {
        return directory_ ? "sandbox-directory-mirror"
                          : "sandbox-file-mirror";
    }

    const char* usage() const override {
        return directory_
                   ? "sandbox-directory-mirror(op, source, mirror?, new_path?, "
                     "explanation?) — op=in copies a readable external "
                     "directory (source) into the workspace sandbox as mirror "
                     "(default workspace/mirrors/<name>); op=apply writes that "
                     "mirror back to its origin after the user chooses replace, "
                     "new, or DO NOT APPLY; op=drop discards the mirror. Hidden "
                     "entries and symlinks are skipped when copying in. The "
                     "old name 'path' is still accepted for source and mirror."
                   : "sandbox-file-mirror(op, source, mirror?, new_path?, "
                     "explanation?) — op=in copies a readable external file "
                     "(source) into the workspace sandbox as mirror (default "
                     "workspace/mirrors/<name>); op=apply writes that mirror "
                     "back to its origin after the user chooses replace, new, "
                     "or DO NOT APPLY; op=drop discards the mirror. The old "
                     "name 'path' is still accepted for source and mirror.";
    }

    action::Result run(
        const std::map<std::string, std::string>& args) const override {
        const std::string op = argText(args, "op");
        if (op == "in") return mirrorIn(directory_, args);
        if (op == "apply") return mirrorApply(directory_, args);
        if (op == "drop") return mirrorDrop(directory_, args);
        action::Result r;
        r.output = failure("op must be in, apply or drop");
        return r;
    }

  private:
    bool directory_;
};

MirrorAction g_file(false);
MirrorAction g_directory(true);

} // namespace

namespace sandbox_mirror {

const action::Action* fileTool() { return &g_file; }
const action::Action* directoryTool() { return &g_directory; }

Pending pending() {
    std::lock_guard<std::mutex> lock(p.mutex);
    Pending out;
    out.active = p.pending;
    if (out.active) {
        out.directory = p.directory;
        out.origin = p.origin;
        out.mirror = p.mirror;
        out.replaceDestination = p.replaceDestination;
        out.newDestination = p.newDestination;
        out.explanation = p.explanation;
        out.id = p.pendingId;
    }
    return out;
}

bool hasPending() {
    std::lock_guard<std::mutex> lock(p.mutex);
    return p.pending;
}

bool decide(ApplyChoice choice) {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.pending) return false;
    p.pending = false;
    p.hasDecision = true;
    p.choice = choice;
    p.reason.clear();
    p.cv.notify_all();
    return true;
}

void cancel(const std::string& reason) {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.pending) return;
    p.pending = false;
    p.hasDecision = true;
    p.choice = ApplyChoice::Deny;
    p.reason = reason.empty() ? "the mirror apply request was cancelled"
                              : reason;
    p.cv.notify_all();
}

} // namespace sandbox_mirror
