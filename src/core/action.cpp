#include "aichat/core/action.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/experience.h"
#include "aichat/core/sandbox.h"
#include "aichat/core/story.h"
#include "aichat/core/util.h"
#include "ai-tools/local-dir-access/local-dir-access.h"
#include "ai-tools/sandbox-mirror/sandbox-mirror.h"
#include "ai-tools/self-made-tool/self-made-tool.h"
#include "ai-tools/text-edit/text-edit.h"
#include "ai-tools/text-grep/text-grep.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace {

constexpr size_t kDefaultRead = 4000;
constexpr size_t kMaxRead = 12000;
constexpr size_t kMaxList = 300;
constexpr size_t kMaxEditBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxGrepBytes = 8u * 1024u * 1024u;

std::atomic<unsigned long long> g_runSeq{0};
thread_local std::string g_lessonTarget;

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string safeFilePart(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            out += static_cast<char>(c);
        else
            out += '_';
    }
    if (out.empty() || out == "." || out == "..") out = "script";
    return out;
}

bool ensureDirectory(const std::string& path) {
    if (::mkdir(path.c_str(), 0700) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
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
    const std::string home = aiChatHome();
    if (path == "~") return home.empty() ? path : home;
    if (startsWith(path, "~/") && !home.empty())
        return home + path.substr(1);
    return path;
}

std::string cwdPath() {
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof buf)) return ".";
    return normalizePath(buf);
}

// Resolve symlinks before any scope check. Without this, a symlink inside an
// allowed root could point at ~/.ssh or another file outside the root.
std::string canonicalPath(const std::string& path) {
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) return "";
    return normalizePath(buf);
}

bool inside(const std::string& root, const std::string& path) {
    if (path == root) return true;
    if (path.size() <= root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    return path[root.size()] == '/' || root == "/";
}

bool isRegularFile(const std::string& path, struct stat& st) {
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensureDir(const std::string& dir) {
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

std::string parentOf(const std::string& path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// True when any component of `path` (relative to `root`) is hidden. This
// keeps an autonomous agent from wandering into ~/.ssh, ~/.aws, .env or
// similar files just because ai-chat was launched from the home directory.
bool hasHiddenComponent(const std::string& root, const std::string& path) {
    if (!inside(root, path)) return false;
    size_t i = path.size() > root.size() ? root.size() : path.size();
    if (i < path.size() && path[i] == '/') ++i;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i && path[i] == '.') return true;
        i = j + 1;
    }
    return false;
}

// Readable roots: the AI's workspace, the self-made-tools tree, the directory
// ai-chat was launched from, and tool folders granted by add/edit (restored
// from the .grants manifest on startup). The rest of the config directory is
// deliberately excluded (it holds the API key). Hidden paths outside those
// roots are refused as a simple secret-store guard.
bool readable(const std::string& path) {
    std::string ws = canonicalPath(action::workspacePath());
    if (ws.empty()) ws = normalizePath(action::workspacePath());
    const std::string cwd = canonicalPath(cwdPath());
    const std::string cfg = canonicalPath(configDirPath());
    if (cwd.empty() || cfg.empty()) return false;

    if (inside(ws, path)) return true;
    // A self-made tool directory is readable after add/edit granted it. The
    // grant may have been restored from the manifest at startup.
    if (!self_made_tools::grantedRoot(path).empty()) return true;
    // Installed tools are readable even without a session grant, so the AI
    // can rediscover them after a restart. Writes and script runs still need
    // the grant restored from the manifest or approved by the user.
    std::string tools = canonicalPath(self_made_tools::rootPath());
    if (tools.empty()) tools = normalizePath(self_made_tools::rootPath());
    if (inside(tools, path)) return !hasHiddenComponent(tools, path);
    // The config directory holds the API key. Never expose it, even when a
    // broader directory was granted.
    if (inside(cfg, path)) return false;

    const std::string granted = local_dir_access::grantedRoot(path);
    if (!granted.empty()) return !hasHiddenComponent(granted, path);

    if (!inside(cwd, path)) return false;
    return !hasHiddenComponent(cwd, path);
}

enum class ReadStatus { Ok, Missing, Forbidden };

struct ReadResolution {
    ReadStatus status = ReadStatus::Missing;
    std::string candidate;
    std::string resolved;
};

// Resolves a read path and says why it failed, so callers can distinguish a
// typo from a path that only needs local_dir_access.
ReadResolution resolveReadChecked(const std::string& raw) {
    ReadResolution out;
    std::string p = expandHome(trim(raw));
    if (p.empty()) return out;
    if (p[0] == '/') {
        out.candidate = normalizePath(p);
    } else {
        const std::string ws = action::workspacePath();
        const std::string inWs = normalizePath(ws + "/" + p);
        struct stat st;
        if (::stat(inWs.c_str(), &st) == 0)
            out.candidate = inWs;
        else
            out.candidate = normalizePath(cwdPath() + "/" + p);
    }
    const std::string canonical = canonicalPath(out.candidate);
    if (canonical.empty()) return out;
    out.resolved = canonical;
    out.status = readable(canonical) ? ReadStatus::Ok : ReadStatus::Forbidden;
    return out;
}

std::string resolveRead(const std::string& raw) {
    const ReadResolution r = resolveReadChecked(raw);
    return r.status == ReadStatus::Ok ? r.resolved : "";
}

std::string readProblem(const std::string& raw) {
    const ReadResolution r = resolveReadChecked(raw);
    switch (r.status) {
    case ReadStatus::Ok:
        return "";
    case ReadStatus::Missing:
        return "no such file or directory: " + r.candidate;
    case ReadStatus::Forbidden:
        return "the path exists but is outside the readable roots: " +
               r.resolved +
               "\nuse local_dir_access to request read access to its directory";
    }
    return "";
}

// Writes must stay inside the workspace or inside a self-made tool directory
// granted by an approved add/edit (including grants restored from the
// manifest). Resolve the parent directory to make sure a symlinked
// subdirectory cannot smuggle a write outside those roots.
std::string resolveWrite(const std::string& raw, std::string& error) {
    std::string p = expandHome(trim(raw));
    if (p.empty()) {
        error = "missing path";
        return "";
    }
    const std::string ws = normalizePath(action::workspacePath());
    if (!ensureDir(ws)) {
        error = "cannot create the workspace directory";
        return "";
    }
    std::string candidate =
        p[0] == '/' ? normalizePath(p) : normalizePath(ws + "/" + p);
    if (candidate == ws) {
        error = "a file path is required";
        return "";
    }

    // Check the lexical location before creating any parent directory. The
    // canonical check after ensureDir() catches symlink escapes.
    if (!inside(ws, candidate) &&
        self_made_tools::grantedRoot(candidate).empty()) {
        error = "writes are limited to the workspace and self-made tool "
                "directories";
        return "";
    }

    struct stat st;
    if (::lstat(candidate.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        error = "refusing to write through a symbolic link";
        return "";
    }

    const std::string parent = parentOf(candidate);
    if (!ensureDir(parent)) {
        error = "cannot create the parent directory";
        return "";
    }
    char realParent[PATH_MAX];
    char realWs[PATH_MAX];
    if (!::realpath(parent.c_str(), realParent) ||
        !::realpath(ws.c_str(), realWs)) {
        error = "cannot resolve the target or workspace path";
        return "";
    }
    const std::string canonicalParent = normalizePath(realParent);
    const bool inWorkspace =
        inside(normalizePath(realWs), canonicalParent);
    const bool inSelfMade =
        !self_made_tools::grantedRoot(canonicalParent).empty();
    if (!inWorkspace && !inSelfMade) {
        error = "the parent directory escapes the workspace and self-made "
                "tool directories";
        return "";
    }
    return candidate;
}

long long numberArg(const std::map<std::string, std::string>& args,
                    const char* name, long long fallback) {
    auto it = args.find(name);
    if (it == args.end()) return fallback;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(it->second.c_str(), &end, 10);
    if (errno != 0 || end == it->second.c_str() || v < 0) return fallback;
    return v;
}

std::string pathArg(const std::map<std::string, std::string>& args) {
    auto it = args.find("path");
    return it == args.end() ? std::string() : trim(it->second);
}

std::string contentArg(const std::map<std::string, std::string>& args) {
    auto it = args.find("content");
    return it == args.end() ? std::string() : it->second;
}

std::string textArg(const std::map<std::string, std::string>& args,
                    const char* name) {
    auto it = args.find(name);
    return it == args.end() ? std::string() : trim(it->second);
}

// Like textArg(), but byte-exact: leading and trailing whitespace can be part
// of the text being matched or inserted, so trimming would change the edit.
std::string rawArg(const std::map<std::string, std::string>& args,
                   const char* name) {
    auto it = args.find(name);
    return it == args.end() ? std::string() : it->second;
}

// Space-separated arguments. There is deliberately no shell parsing here:
// sandbox-run-script accepts a plain argument list, not a command line.
std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

std::string firstLine(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    line = text_utf8::sanitize(line);
    if (line.size() > 512) line.resize(text_utf8::headCut(line, 512));
    return line;
}

// Parses "#!interpreter [arg...]" into an argv prefix.
std::vector<std::string> shebangCommand(const std::string& line) {
    if (line.size() < 3 || line[0] != '#' || line[1] != '!')
        return std::vector<std::string>();
    return splitWords(line.substr(2));
}

std::string lowerAscii(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string fileExtension(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash))
        return "";
    return lowerAscii(path.substr(dot));
}

std::vector<std::string> interpreterForExtension(const std::string& ext) {
    if (ext == ".py") return {"/usr/bin/env", "python3"};
    if (ext == ".sh") return {"/bin/sh"};
    if (ext == ".bash") return {"/bin/bash"};
    if (ext == ".zsh") return {"/usr/bin/env", "zsh"};
    if (ext == ".fish") return {"/usr/bin/env", "fish"};
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs")
        return {"/usr/bin/env", "node"};
    if (ext == ".rb") return {"/usr/bin/env", "ruby"};
    if (ext == ".pl") return {"/usr/bin/env", "perl"};
    if (ext == ".php") return {"/usr/bin/env", "php"};
    if (ext == ".lua") return {"/usr/bin/env", "lua"};
    if (ext == ".r") return {"/usr/bin/env", "Rscript"};
    if (ext == ".jl") return {"/usr/bin/env", "julia"};
    return {};
}

std::string failure(const std::string& message) { return "error: " + message; }

// ---------------------------------------------------------------------------

class ReadFileAction : public action::Action {
  public:
    const char* name() const override { return "read_file"; }
    const char* usage() const override {
        return "read_file(path, offset?, limit?) — read up to 4000 bytes "
               "(limit is capped at 12000) of a UTF-8 text file";
    }
    action::Result run(const std::map<std::string, std::string>& args) const override {
        action::Result r;
        std::string path = pathArg(args);
        if (path.empty()) {
            r.output = failure("read_file needs a path");
            return r;
        }
        std::string resolved = resolveRead(path);
        if (resolved.empty()) {
            r.output = failure("path is not readable; use a file in the "
                               "workspace, the self-made-tools directory, or "
                               "the directory ai-chat was launched from");
            return r;
        }
        struct stat st;
        if (!isRegularFile(resolved, st)) {
            r.output = failure("not a regular file: " + resolved);
            return r;
        }

        long long offset = numberArg(args, "offset", 0);
        size_t limit = static_cast<size_t>(numberArg(args, "limit", kDefaultRead));
        if (limit == 0) limit = kDefaultRead;
        if (limit > kMaxRead) limit = kMaxRead;
        if (offset < 0) offset = 0;
        if (offset > static_cast<long long>(st.st_size))
            offset = static_cast<long long>(st.st_size);

        std::ifstream in(resolved, std::ios::binary);
        if (!in) {
            r.output = failure("cannot open " + resolved);
            return r;
        }
        in.seekg(static_cast<std::streamoff>(offset));
        // Read a few extra bytes so the cut can be moved back to a code-point
        // boundary instead of splitting a multi-byte character.
        std::string data(limit + 4, '\0');
        in.read(&data[0], static_cast<std::streamsize>(limit + 4));
        data.resize(static_cast<size_t>(in.gcount()));
        bool truncated = data.size() > limit;
        if (truncated) data.resize(text_utf8::headCut(data, limit));

        if (data.find('\0') != std::string::npos) {
            r.output = failure("binary file (contains NUL bytes): " + resolved);
            return r;
        }

        size_t repaired = 0;
        data = text_utf8::sanitize(data, &repaired);

        std::ostringstream out;
        out << "path: " << resolved << "\n"
            << "offset: " << offset << "\n"
            << "total: " << st.st_size << " bytes\n"
            << "shown: " << data.size() << " bytes"
            << (repaired ? " (replaced " + std::to_string(repaired) +
                               " invalid UTF-8 sequence(s))"
                         : "")
            << (truncated ? " (truncated; continue with a larger offset)" : "")
            << "\n";
        if (truncated)
            out << "next_offset: " << (offset + static_cast<long long>(data.size()))
                << "\n";
        out << "---\n" << data;
        if (!data.empty() && data.back() != '\n') out << "\n";
        r.ok = true;
        r.output = out.str();
        r.facts.push_back(facts::make(
            facts::FactKind::FsFile, "file:" + resolved, resolved,
            "exists, " + std::to_string(static_cast<long long>(st.st_size)) +
                " bytes",
            facts::FactStatus::Verified, "stat"));
        return r;
    }
};

class ListDirAction : public action::Action {
  public:
    const char* name() const override { return "list_dir"; }
    const char* usage() const override {
        return "list_dir(path?, limit?) — list up to 200 entries in a "
               "readable directory";
    }
    action::Result run(const std::map<std::string, std::string>& args) const override {
        action::Result r;
        std::string path = pathArg(args);
        if (path.empty()) path = ".";
        std::string resolved = resolveRead(path);
        if (resolved.empty()) {
            r.output = failure("path is not readable; use the workspace, the "
                               "self-made-tools directory, or the directory "
                               "ai-chat was launched from");
            return r;
        }
        if (!isDirectory(resolved)) {
            r.output = failure("not a directory: " + resolved);
            return r;
        }
        size_t limit = static_cast<size_t>(numberArg(args, "limit", 100));
        if (limit == 0 || limit > kMaxList) limit = kMaxList;

        DIR* dir = ::opendir(resolved.c_str());
        if (!dir) {
            r.output = failure("cannot open directory " + resolved);
            return r;
        }
        struct Entry {
            std::string name;
            bool isDir;
            long long size;
        };
        const bool inWorkspace =
            inside(canonicalPath(action::workspacePath()), resolved);
        std::vector<Entry> entries;
        while (struct dirent* ent = ::readdir(dir)) {
            // Filesystem names are arbitrary bytes; make them safe before they
            // reach a prompt or the fact store.
            std::string name = text_utf8::sanitize(ent->d_name);
            if (name == "." || name == "..") continue;
            // Do not advertise secret-store entries from the launch
            // directory; the workspace can contain whatever the user put
            // there.
            if (!inWorkspace && !name.empty() && name[0] == '.') continue;
            std::string full = resolved + "/" + name;
            struct stat st;
            if (::stat(full.c_str(), &st) != 0) continue;
            entries.push_back(
                {name, S_ISDIR(st.st_mode), static_cast<long long>(st.st_size)});
        }
        ::closedir(dir);
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.name < b.name; });

        std::ostringstream out;
        out << "path: " << resolved << "\n"
            << "entries: " << entries.size();
        if (entries.size() > limit) out << " (showing " << limit << ")";
        out << "\n---\n";
        for (size_t i = 0; i < entries.size() && i < limit; ++i) {
            out << entries[i].name;
            if (entries[i].isDir) out << "/";
            else out << "  (" << entries[i].size << " bytes)";
            out << "\n";
        }
        std::string listed;
        for (size_t i = 0; i < entries.size() && i < limit; ++i) {
            if (!listed.empty()) listed += ", ";
            listed += entries[i].name;
            if (entries[i].isDir)
                listed += "/";
            else
                listed +=
                    " (" + std::to_string(entries[i].size) + " bytes)";
            if (listed.size() > 1200) {
                listed += "…";
                break;
            }
        }
        std::string value = "entries: " + std::to_string(entries.size());
        if (entries.size() > limit)
            value += " (showing " + std::to_string(limit) + ")";
        if (!listed.empty()) value += "; " + listed;
        r.facts.push_back(facts::make(
            facts::FactKind::Listing, "dir:" + resolved, resolved, value,
            facts::FactStatus::Verified, "readdir"));

        r.ok = true;
        r.output = out.str();
        return r;
    }
};

class WriteFileAction : public action::Action {
  public:
    bool append_;
    explicit WriteFileAction(bool append) : append_(append) {}
    const char* name() const override { return append_ ? "append_file" : "write_file"; }
    const char* usage() const override {
        return append_
                   ? "append_file(path, content) — append UTF-8 text to a file "
                     "in the sandbox workspace or a granted self-made tool "
                     "folder"
                   : "write_file(path, content) — create or overwrite a UTF-8 "
                     "text file in the sandbox workspace or a granted "
                     "self-made tool folder";
    }
    action::Result run(const std::map<std::string, std::string>& args) const override {
        action::Result r;
        std::string path = pathArg(args);
        if (path.empty()) {
            r.output = failure(std::string(name()) + " needs a path");
            return r;
        }
        std::string error;
        std::string resolved = resolveWrite(path, error);
        if (resolved.empty()) {
            r.output = failure(error);
            return r;
        }

        const std::string content = contentArg(args);
        std::ofstream out(resolved,
                          std::ios::binary |
                              (append_ ? std::ios::app : std::ios::trunc));
        if (!out) {
            r.output = failure("cannot open " + resolved + " for writing");
            return r;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) {
            r.output = failure("could not write " + resolved);
            return r;
        }
        r.ok = true;
        const std::string verb = append_ ? "appended " : "wrote ";
        r.output = verb + std::to_string(content.size()) + " bytes to " +
                   resolved;
        r.facts.push_back(facts::make(
            facts::FactKind::Wrote, "file:" + resolved, resolved,
            verb + std::to_string(content.size()) + " bytes",
            facts::FactStatus::Reported, "stat"));
        return r;
    }
};

// Reads a whole regular file for the line-oriented tools below. Returns an
// empty string and fills `error` on failure or when the file is too large.
std::string readWholeFile(const std::string& path, size_t maxBytes,
                          std::string& error) {
    struct stat st;
    if (!isRegularFile(path, st)) {
        error = "not a regular file: " + path;
        return "";
    }
    if (static_cast<size_t>(st.st_size) > maxBytes) {
        error = "file is larger than " + std::to_string(maxBytes) +
                " bytes; use read_file with an offset instead";
        return "";
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return "";
    }
    std::string data(static_cast<size_t>(st.st_size), '\0');
    if (!data.empty()) in.read(&data[0], static_cast<std::streamsize>(data.size()));
    if (!in && !in.eof()) {
        error = "could not read " + path;
        return "";
    }
    if (data.find('\0') != std::string::npos) {
        error = "binary file (contains NUL bytes): " + path;
        return "";
    }
    return data;
}

class EditFileAction : public action::Action {
  public:
    const char* name() const override { return "edit_file"; }
    const char* usage() const override {
        return "edit_file(path, old, new) — replace the single occurrence of "
               "`old` with `new` in a UTF-8 text file; fails when `old` is "
               "missing or appears more than once";
    }
    action::Result run(
        const std::map<std::string, std::string>& args) const override {
        action::Result r;
        const std::string path = pathArg(args);
        if (path.empty()) {
            r.output = failure("edit_file needs a path");
            return r;
        }
        const std::string oldText = rawArg(args, "old");
        const std::string newText = rawArg(args, "new");
        if (oldText.empty()) {
            r.output = failure("edit_file needs a non-empty `old` argument");
            return r;
        }

        std::string error;
        const std::string resolved = resolveWrite(path, error);
        if (resolved.empty()) {
            r.output = failure(error);
            return r;
        }
        const std::string source = readWholeFile(resolved, kMaxEditBytes, error);
        if (source.empty() && !error.empty()) {
            r.output = failure(error);
            return r;
        }

        const text_edit::Result edited =
            text_edit::replaceOnce(source, oldText, newText);
        if (!edited.ok) {
            r.output = failure(edited.error);
            return r;
        }

        // Write through a sibling temp file and rename, so a failed write
        // cannot leave a half-written document behind.
        struct stat before;
        ::stat(resolved.c_str(), &before);
        const std::string tmp = resolved + ".aichat-edit.tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                r.output = failure("cannot create a temporary file beside " +
                                   resolved);
                return r;
            }
            out.write(edited.text.data(),
                      static_cast<std::streamsize>(edited.text.size()));
            out.close();
            if (!out) {
                ::unlink(tmp.c_str());
                r.output = failure("could not write the edited file");
                return r;
            }
        }
        if (::chmod(tmp.c_str(), before.st_mode & 0777) != 0) { /* keep umask */
        }
        if (::rename(tmp.c_str(), resolved.c_str()) != 0) {
            ::unlink(tmp.c_str());
            r.output = failure("could not replace " + resolved);
            return r;
        }

        std::ostringstream out;
        out << "edited: " << resolved << "\n"
            << "offset: " << edited.offset << " (line " << edited.line << ")\n"
            << "replaced: " << oldText.size() << " bytes with "
            << newText.size() << " bytes\n"
            << "file now: " << edited.text.size() << " bytes\n";
        r.ok = true;
        r.output = out.str();
        r.facts.push_back(facts::make(
            facts::FactKind::Wrote, "file:" + resolved, resolved,
            "edited at byte " + std::to_string(edited.offset) + " (line " +
                std::to_string(edited.line) + "), " +
                std::to_string(edited.text.size()) + " bytes",
            facts::FactStatus::Verified, "content"));
        return r;
    }
};

class GrepFileAction : public action::Action {
  public:
    const char* name() const override { return "grep_file"; }
    const char* usage() const override {
        return "grep_file(path, pattern, limit?) — find literal occurrences of "
               "`pattern` in a readable UTF-8 text file; prints line number, "
               "byte offset and the matching line";
    }
    action::Result run(
        const std::map<std::string, std::string>& args) const override {
        action::Result r;
        const std::string path = pathArg(args);
        if (path.empty()) {
            r.output = failure("grep_file needs a path");
            return r;
        }
        const std::string pattern = rawArg(args, "pattern");
        if (pattern.empty()) {
            r.output = failure("grep_file needs a non-empty `pattern`");
            return r;
        }
        const std::string resolved = resolveRead(path);
        if (resolved.empty()) {
            r.output = failure("path is not readable; use a file in the "
                               "workspace, the self-made-tools directory, or "
                               "the directory ai-chat was launched from");
            return r;
        }

        std::string error;
        const std::string data = readWholeFile(resolved, kMaxGrepBytes, error);
        if (data.empty() && !error.empty()) {
            r.output = failure(error);
            return r;
        }

        size_t limit = static_cast<size_t>(numberArg(args, "limit", 50));
        if (limit == 0) limit = 50;
        if (limit > 500) limit = 500;
        const text_grep::Result found = text_grep::find(data, pattern, limit);

        std::ostringstream out;
        out << "path: " << resolved << "\n"
            << "pattern: " << text_utf8::head(pattern, 200) << "\n"
            << "matches: " << found.total;
        if (found.total > found.matches.size())
            out << " (showing " << found.matches.size() << ")";
        out << "\n---\n";
        for (const text_grep::Match& m : found.matches) {
            out << m.line << ":" << m.offset << ":" << m.text << "\n";
        }
        r.ok = true;
        r.output = out.str();

        std::string value = "no match";
        if (found.total > 0) {
            value = std::to_string(found.total) + " match" +
                    (found.total == 1 ? "" : "es") + "; first at line " +
                    std::to_string(found.matches.front().line) + ", byte " +
                    std::to_string(found.matches.front().offset);
        }
        r.facts.push_back(facts::make(
            facts::FactKind::Generic,
            "grep:" + resolved + ":" + text_utf8::head(pattern, 80), resolved,
            value, facts::FactStatus::Verified, "content"));
        return r;
    }
};

class ThinkAction : public action::Action {
  public:
    const char* name() const override { return "think"; }
    const char* usage() const override {
        return "think(note) — record a reasoning-only step; changes nothing "
               "outside the loop's journal";
    }
    action::Result run(const std::map<std::string, std::string>& args) const override {
        auto it = args.find("note");
        action::Result r;
        r.ok = true;
        r.output = it == args.end() || trim(it->second).empty()
                       ? "recorded a reasoning step"
                       : "recorded: " + trim(it->second);
        return r;
    }
};

class LessonAction : public action::Action {
  public:
    const char* name() const override { return "lesson"; }
    const char* usage() const override {
        return "lesson(text) — record one reusable practice lesson in this "
               "target's .exp through the app (purpose -> practice -> "
               "evidence); no file path or directory access needed";
    }
    action::Result run(const std::map<std::string, std::string>& args) const override {
        action::Result r;
        auto it = args.find("text");
        if (it == args.end()) it = args.find("body");
        if (it == args.end() || trim(it->second).empty()) {
            r.output = failure("lesson needs a non-empty text");
            return r;
        }
        if (g_lessonTarget.empty()) {
            r.output = failure(
                "lesson is only available while working on a target");
            return r;
        }
        const std::string path =
            experience::recordLesson(g_lessonTarget, trim(it->second));
        if (path.empty()) {
            r.output = failure("could not record the lesson");
            return r;
        }
        r.ok = true;
        r.output = "recorded lesson in " + path;
        r.facts.push_back(facts::make(
            facts::FactKind::Wrote, "file:" + path, path,
            "recorded a practice lesson", facts::FactStatus::Reported,
            "stat"));
        return r;
    }
};

class StoryAction : public action::Action {
  public:
    const char* name() const override { return "story"; }
    const char* usage() const override {
        return "story(text, object?) — replace the life story, or one object "
               "story when object is given; keep the Live / Arc / Chapter / "
               "Boundary layers meaningful, not a log";
    }
    action::Result run(const std::map<std::string, std::string>& args) const override {
        action::Result r;
        auto it = args.find("text");
        if (it == args.end()) it = args.find("body");
        if (it == args.end() || trim(it->second).empty()) {
            r.output = failure("story needs a non-empty text");
            return r;
        }
        const auto obj = args.find("object");
        const std::string object =
            obj == args.end() ? std::string() : trim(obj->second);
        const bool ok = object.empty()
                            ? story::writeLife(trim(it->second))
                            : story::writeObject(object, trim(it->second));
        const std::string path =
            object.empty() ? story::lifePath() : story::objectPath(object);
        if (!ok || path.empty()) {
            r.output = failure("could not update story memory");
            return r;
        }
        r.ok = true;
        r.output = "updated story memory in " + path;
        r.facts.push_back(facts::make(
            facts::FactKind::Wrote, "file:" + path, path,
            "updated story memory", facts::FactStatus::Reported, "stat"));
        return r;
    }
};

// Script output is often binary (a PDF extractor dumping raw glyph codes).
// Dumping it into the prompt wastes context and `read_file` refuses the saved
// artifact, so the run result shows a short escaped preview instead.
bool mostlyBinary(const std::string& s) {
    if (s.find('\0') != std::string::npos) return true;
    const size_t sample = std::min<size_t>(s.size(), 4096);
    if (sample == 0) return false;
    size_t bad = 0;
    for (size_t i = 0; i < sample; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 0x20 || c == 0x7f) ++bad;
    }
    return bad * 10 > sample;
}

std::string escapedPreview(const std::string& s, size_t maxBytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    const size_t n = std::min(s.size(), maxBytes);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c >= 0x20 && c < 0x7f) {
            out += static_cast<char>(c);
        } else {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 0xf];
        }
    }
    if (n < s.size()) out += "...";
    return out;
}

class SandboxRunScriptAction : public action::Action {
  public:
    const char* name() const override { return "sandbox-run-script"; }

    const char* usage() const override {
        return "sandbox-run-script(path, args?, timeout_sec?) — run a "
               "script from the workspace or a granted self-made tool folder "
               "in the sandbox; args is a space-separated argument list; no "
               "network, 15s default timeout (max 120s), output is combined "
               "stdout and stderr. The sandbox runs the script with the "
               "workspace as its working directory and exports that path as "
               "AI_CHAT_WORKSPACE; host absolute paths such as /home/... are "
               "not visible, so use relative paths or the environment "
               "variable";
    }

    action::Result run(
        const std::map<std::string, std::string>& args) const override {
        action::Result r;
        const std::string path = pathArg(args);
        if (path.empty()) {
            r.output = failure("sandbox-run-script needs a path");
            return r;
        }

        const std::string resolved = resolveRead(path);
        if (resolved.empty()) {
            r.output = failure("path is not readable: " + path);
            return r;
        }

        // The script's root is also the sandbox's writable workdir, so only
        // roots that are already writable may be used.
        const std::string ws = normalizePath(action::workspacePath());
        std::string root;
        if (inside(ws, resolved))
            root = ws;
        else
            root = self_made_tools::grantedRoot(resolved);
        if (root.empty()) {
            r.output = failure(
                "sandbox-run-script can only run files inside the workspace "
                "or an approved self-made tool folder");
            return r;
        }

        struct stat st;
        if (!isRegularFile(resolved, st)) {
            r.output = failure("not a regular file: " + resolved);
            return r;
        }

        const std::string relative = resolved.substr(root.size() + 1);
        std::vector<std::string> command;

        // An executable file runs directly (this also covers compiled
        // tools). Otherwise use its shebang, or a known file extension.
        if (::access(resolved.c_str(), X_OK) == 0) {
            command.push_back("./" + relative);
        } else {
            command = shebangCommand(firstLine(resolved));
            if (command.empty())
                command = interpreterForExtension(fileExtension(resolved));
            if (command.empty()) {
                r.output = failure(
                    "cannot determine how to run " + relative +
                    "; make it executable, add a shebang, or use a known "
                    "extension such as .py or .sh");
                return r;
            }
            command.push_back(relative);
        }

        for (const std::string& arg : splitWords(textArg(args, "args")))
            command.push_back(arg);

        std::string reason;
        if (!sandbox::platform().available(reason)) {
            r.output = failure("sandbox unavailable: " + reason);
            return r;
        }

        long long timeoutSec = numberArg(args, "timeout_sec", 15);
        if (timeoutSec < 1) timeoutSec = 1;
        if (timeoutSec > 120) timeoutSec = 120;

        sandbox::Request request;
        request.command = command;
        request.workdir = root;
        request.limits.wallMs = timeoutSec * 1000;
        const sandbox::Result result = sandbox::platform().run(request);

        size_t repaired = 0;
        const std::string clean =
            text_utf8::sanitize(result.output, &repaired);

        // Keep the output as a durable artifact. The journal only carries a
        // short excerpt, so without this file the model cannot quote what its
        // own run printed after the next prompt is assembled.
        std::string savedPath;
        std::string saveError;
        const std::string runsDir =
            normalizePath(action::workspacePath() + "/runs");
        if (!ensureDirectory(runsDir)) {
            saveError = "cannot create " + runsDir;
        } else {
            std::ostringstream name;
            name << "run-" << static_cast<long long>(getpid()) << "-"
                 << (++g_runSeq) << "-" << safeFilePart(baseName(resolved))
                 << ".out";
            const std::string candidate = runsDir + "/" + name.str();
            std::ofstream f(candidate, std::ios::binary | std::ios::trunc);
            if (!f) {
                saveError = "cannot open " + candidate;
            } else {
                f.write(clean.data(), static_cast<std::streamsize>(clean.size()));
                f.close();
                if (!f)
                    saveError = "could not write " + candidate;
                else
                    savedPath = candidate;
            }
        }

        std::ostringstream out;
        out << "script: " << relative << "\n"
            << "exit: " << result.exitCode;
        if (result.timedOut) out << " (timed out)";
        if (result.truncated) out << " (output truncated)";
        out << "\n";
        if (repaired)
            out << "utf8: replaced " << repaired
                << " invalid sequence(s) in the script output\n";
        if (!saveError.empty()) out << "save: " << saveError << "\n";
        else out << "saved: " << savedPath << "\n";
        if (!result.error.empty()) out << "sandbox: " << result.error << "\n";
        out << "---\n";
        if (!clean.empty()) {
            if (mostlyBinary(clean)) {
                const size_t nuls = static_cast<size_t>(
                    std::count(clean.begin(), clean.end(), '\0'));
                out << "[binary output: " << result.output.size()
                    << " bytes";
                if (nuls) out << ", " << nuls << " NUL";
                out << "; escaped head follows]\n"
                    << escapedPreview(clean, 600) << "\n";
            } else {
                out << clean;
                if (clean.back() != '\n') out << "\n";
            }
        }

        r.ok = result.ok;
        r.output = out.str();
        std::string factValue =
            "exit=" + std::to_string(result.exitCode) +
            (result.timedOut ? " timed out" : "") +
            (result.truncated ? " truncated" : "") +
            "; stdout=" + std::to_string(result.output.size()) + " bytes";
        if (!savedPath.empty()) factValue += "; saved=" + savedPath;
        if (repaired)
            factValue += "; utf8_repaired=" + std::to_string(repaired);
        r.facts.push_back(facts::make(facts::FactKind::Run, "run:" + resolved,
                                      resolved, factValue,
                                      facts::FactStatus::Verified, "waitpid"));
        return r;
    }
};

const ReadFileAction g_read;
const ListDirAction g_list;
const WriteFileAction g_write(false);
const WriteFileAction g_append(true);
const EditFileAction g_edit;
const GrepFileAction g_grep;
const ThinkAction g_think;
const LessonAction g_lesson;
const StoryAction g_story;
const SandboxRunScriptAction g_run;

const action::Action* const g_actions[] = {
    &g_read, &g_list, &g_write, &g_append, &g_edit, &g_grep, &g_think,
    &g_lesson, &g_story, &g_run,
    local_dir_access::tool(),
    self_made_tools::tool(),
    sandbox_mirror::fileTool(),
    sandbox_mirror::directoryTool(),
};

} // namespace

namespace action {

std::string workspacePath() {
    const std::string ws = configSubpath("workspace");
    return ws.empty() ? std::string() : normalizePath(ws);
}

std::string resolveReadablePath(const std::string& raw) {
    return resolveRead(raw);
}

std::string readablePathProblem(const std::string& raw) {
    return readProblem(raw);
}

std::string approvedMirrorWriteRoot(const std::string& path) {
    const std::string cfg = canonicalPath(configDirPath());
    if (!cfg.empty() && inside(cfg, path)) return "";

    const std::string cwd = canonicalPath(cwdPath());
    if (!cwd.empty() && inside(cwd, path)) {
        return hasHiddenComponent(cwd, path) ? std::string() : cwd;
    }

    const std::string granted = local_dir_access::grantedRoot(path);
    if (!granted.empty() && !hasHiddenComponent(granted, path))
        return granted;
    return "";
}

void setLessonTarget(const std::string& target) {
    g_lessonTarget = target;
}

std::string catalog() {
    std::ostringstream out;
    out << "workspace sandbox (writable drafts and experiments; files here "
           "are not installed tools — write a tool's final files in its "
           "self-made-tool folder): "
        << workspacePath() << "\n"
        << "self-made tools (installed with user approval; writable after "
           "add/edit): "
        << self_made_tools::rootPath() << "\n"
        << "tool workflow: self-made-tool(op=add, name=...) then "
           "op=write/append/edit/import/grep/run with paths relative to that "
           "tool folder; use op=list to see what is installed\n"
        << "readable: the workspace, " << cwdPath()
        << ", granted self-made tool directories, and directories approved "
           "through local_dir_access (hidden paths outside those roots are "
           "refused)\n";
    for (const Action* a : g_actions) out << "- " << a->usage() << "\n";
    return out.str();
}

Result execute(const Request& request) {
    for (const Action* a : g_actions) {
        if (request.name != a->name()) continue;
        try {
            return a->run(request.args);
        } catch (...) {
            Result r;
            r.output = failure("the action failed with an internal error");
            return r;
        }
    }
    Result r;
    r.output = failure("unknown action \"" + request.name +
                       "\"; use one of: read_file, list_dir, "
                       "local_dir_access, self-made-tool, sandbox-run-script, "
                       "sandbox-file-mirror, sandbox-directory-mirror, "
                       "write_file, append_file, edit_file, grep_file, think, "
                       "lesson, story");
    return r;
}

} // namespace action
