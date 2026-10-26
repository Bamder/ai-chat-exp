#include "ai-tools/self-made-tool/self-made-tool.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <ftw.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/facts/fact.h"
#include "aichat/core/util.h"
#include "ai-tools/text-grep/text-grep.h"

namespace {

constexpr size_t kMaxToolName = 64;
constexpr size_t kMaxToolFiles = 300;
constexpr size_t kMaxToolFileBytes = 8u * 1024u * 1024u;
constexpr unsigned long long kMaxImportBytes = 64ull * 1024ull * 1024ull;

struct State {
    std::mutex mutex;
    std::vector<std::string> roots;
};

State g;

struct PermissionState {
    std::mutex mutex;
    std::condition_variable cv;

    bool pending = false;
    std::string op;
    std::string name;
    std::string explanation;
    unsigned long long pendingId = 0;
    unsigned long long nextId = 1;

    bool hasDecision = false;
    bool allow = false;
    std::string reason;
};

PermissionState p;

bool inside(const std::string& root, const std::string& path) {
    if (path == root) return true;
    if (path.size() <= root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    return path[root.size()] == '/' || root == "/";
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

std::string canonicalPath(const std::string& path) {
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) return "";
    return buf;
}

std::string nameArg(const std::map<std::string, std::string>& args) {
    auto it = args.find("name");
    return it == args.end() ? std::string() : trim(it->second);
}

std::string opArg(const std::map<std::string, std::string>& args) {
    auto it = args.find("op");
    return it == args.end() ? std::string() : trim(it->second);
}

std::string explanationArg(const std::map<std::string, std::string>& args) {
    auto it = args.find("explanation");
    return it == args.end() ? std::string() : trim(it->second);
}

std::string failure(const std::string& message) { return "error: " + message; }

// Asks the main thread to show an approval prompt. The worker blocks here
// until the user answers, the request times out, or the session cancels it.
bool requestPermission(const std::string& op, const std::string& name,
                       const std::string& explanation, std::string& error) {
    error.clear();
    if (!::isatty(STDIN_FILENO)) {
        error = "changing a self-made tool needs interactive approval from the "
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
    p.op = op;
    p.name = name;
    p.explanation = explanation;
    p.pendingId = id;
    p.hasDecision = false;
    p.allow = false;
    p.reason.clear();
    p.cv.notify_all();

    // No timeout: an unanswered request parks the loop at this step instead
    // of turning into a failed action that the planner retries.
    p.cv.wait(lock, [&] { return !p.pending; });

    const bool allowed = p.hasDecision && p.allow;
    const std::string reason = p.reason;
    p.op.clear();
    p.name.clear();
    p.explanation.clear();
    p.pendingId = 0;
    p.hasDecision = false;
    p.reason.clear();
    lock.unlock();

    if (!allowed) {
        error = reason.empty()
                    ? "the user denied permission to " + op +
                          " self-made tool \"" + name + "\""
                    : reason;
        return false;
    }
    return true;
}

// A tool name becomes one directory component, so it must not be able to
// escape the self-made-tools root or hide itself.
bool validName(const std::string& name) {
    if (name.empty() || name.size() > kMaxToolName || name[0] == '.')
        return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

std::string toolDir(const std::string& name) {
    return self_made_tools::rootPath() + "/" + name;
}

std::string leafName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Requires g.mutex. The manifest stores one tool-directory name per line, so
// even a hand-edited file cannot grant a path outside the tools root.
bool writeGrantsLocked() {
    const std::string root = self_made_tools::rootPath();
    if (!ensureDir(root)) return false;
    const std::string path = root + "/.grants";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const std::string& dir : g.roots) out << leafName(dir) << "\n";
        out.flush();
        if (!out) {
            out.close();
            ::unlink(tmp.c_str());
            return false;
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    ::chmod(path.c_str(), 0600);
    return true;
}

// Returns false when the in-memory grant was applied but the persistent
// manifest could not be written.
bool grant(const std::string& canonicalDir) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (std::find(g.roots.begin(), g.roots.end(), canonicalDir) == g.roots.end())
        g.roots.push_back(canonicalDir);
    return writeGrantsLocked();
}

bool revoke(const std::string& canonicalDir) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.roots.erase(
        std::remove(g.roots.begin(), g.roots.end(), canonicalDir),
        g.roots.end());
    return writeGrantsLocked();
}

int removeEntry(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path) == 0 ? 0 : -1;
}

std::string unavailable(const std::string& name) {
    return "no such self-made tool \"" + name + "\"; use op=add first";
}

std::string manifestWarning() {
    return "\nwarning: the grant is active for this session but could not be "
           "saved to " +
           self_made_tools::rootPath() + "/.grants";
}

std::string argValue(const std::map<std::string, std::string>& args,
                     const char* name) {
    auto it = args.find(name);
    return it == args.end() ? std::string() : it->second;
}

action::Result fail(const std::string& message) {
    action::Result r;
    r.output = failure(message);
    return r;
}

// A tool file path is always relative to the tool directory. Rejecting empty
// components, "." and ".." here keeps a request from climbing out before the
// normal file actions apply their canonical-path checks.
bool relativePathOk(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.size() > 4096) return false;
    if (path.find('\0') != std::string::npos) return false;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        const std::string part = path.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return false;
        if (j == path.size()) break;
        i = j + 1;
    }
    return true;
}

std::string parentOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

struct ToolRef {
    std::string name;
    std::string dir; // canonical path
};

bool resolveTool(const std::string& name, ToolRef& ref, std::string& error) {
    const std::string dir = toolDir(name);
    const std::string canonicalDir = canonicalPath(dir);
    if (canonicalDir.empty() || !isDirectory(canonicalDir)) {
        error = unavailable(name);
        return false;
    }
    const std::string canonicalRoot =
        canonicalPath(self_made_tools::rootPath());
    if (canonicalRoot.empty() || !inside(canonicalRoot, canonicalDir)) {
        error = "\"" + name +
                "\" is not inside the self-made-tools directory";
        return false;
    }
    ref.name = name;
    ref.dir = canonicalDir;
    return true;
}

bool toolFilePath(const ToolRef& ref, const std::string& rel,
                  std::string& full, std::string& error) {
    if (!relativePathOk(rel)) {
        error = "path must be relative to the tool directory and must not "
                "start with '/' or contain '.' or '..' components";
        return false;
    }
    full = ref.dir + "/" + rel;
    return true;
}

// Parent components must be real directories, not symlinks: a tool-local link
// could otherwise redirect an import or write outside the tool folder before
// the canonical-path check runs.
bool checkParentComponents(const std::string& base, const std::string& rel,
                           std::string& error) {
    size_t i = 0;
    while (i < rel.size()) {
        const size_t j = rel.find('/', i);
        if (j == std::string::npos) break;
        const std::string dir = base + "/" + rel.substr(0, j);
        struct stat st;
        if (::lstat(dir.c_str(), &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                error = "refusing to write through the symbolic link " + dir;
                return false;
            }
            if (!S_ISDIR(st.st_mode)) {
                error = "not a directory: " + dir;
                return false;
            }
        }
        i = j + 1;
    }
    return true;
}

bool readTextFile(const std::string& path, size_t maxBytes,
                  std::string& data, std::string& error) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "not a regular file: " + path;
        return false;
    }
    if (static_cast<unsigned long long>(st.st_size) > maxBytes) {
        error = "file is larger than " + std::to_string(maxBytes) +
                " bytes: " + path;
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    data.assign(static_cast<size_t>(st.st_size), '\0');
    if (!data.empty())
        in.read(&data[0], static_cast<std::streamsize>(data.size()));
    if (!in && !in.eof()) {
        error = "could not read " + path;
        return false;
    }
    if (data.find('\0') != std::string::npos) {
        error = "binary file (contains NUL bytes): " + path;
        return false;
    }
    return true;
}

struct FileEntry {
    std::string rel;
    long long size = 0;
};

void collectFiles(const std::string& dir, const std::string& prefix,
                  std::vector<FileEntry>& out) {
    if (out.size() >= kMaxToolFiles) return;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name.empty() || name == "." || name == ".." || name[0] == '.')
            continue;
        const std::string full = dir + "/" + name;
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        struct stat st;
        if (::lstat(full.c_str(), &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue; // never follow a tool-local link
        if (S_ISDIR(st.st_mode)) {
            collectFiles(full, rel, out);
        } else if (S_ISREG(st.st_mode)) {
            out.push_back({rel, static_cast<long long>(st.st_size)});
        }
        if (out.size() >= kMaxToolFiles) break;
    }
    ::closedir(d);
}

bool copyFileInto(const std::string& src, const std::string& dst,
                  long long& bytes, std::string& error) {
    struct stat st;
    if (::lstat(src.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "not a regular file: " + src;
        return false;
    }
    if (static_cast<unsigned long long>(st.st_size) > kMaxImportBytes) {
        error = "file is larger than " + std::to_string(kMaxImportBytes) +
                " bytes: " + src;
        return false;
    }
    struct stat dstSt;
    if (::lstat(dst.c_str(), &dstSt) == 0) {
        if (S_ISLNK(dstSt.st_mode)) {
            error = "refusing to overwrite the symbolic link " + dst;
            return false;
        }
        if (S_ISDIR(dstSt.st_mode)) {
            error = "the destination is a directory: " + dst;
            return false;
        }
    }

    std::ifstream in(src, std::ios::binary);
    if (!in) {
        error = "cannot open " + src;
        return false;
    }
    const std::string tmp = dst + ".aichat-import.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot create " + tmp;
            return false;
        }
        char buf[65536];
        while (in) {
            in.read(buf, sizeof buf);
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            out.write(buf, got);
            bytes += got;
        }
        out.close();
        if (!in.eof() || !out) {
            ::unlink(tmp.c_str());
            error = "could not copy " + src + " to " + dst;
            return false;
        }
    }
    ::chmod(tmp.c_str(), st.st_mode & 0777);
    if (::rename(tmp.c_str(), dst.c_str()) != 0) {
        ::unlink(tmp.c_str());
        error = "could not replace " + dst;
        return false;
    }
    return true;
}

bool copyTreeInto(const std::string& src, const std::string& dst,
                  long long& bytes, size_t& files, std::string& error) {
    struct stat st;
    if (::lstat(src.c_str(), &st) != 0) {
        error = "cannot inspect " + src;
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        error = "refusing to copy the symbolic link " + src;
        return false;
    }
    if (S_ISREG(st.st_mode)) {
        if (!copyFileInto(src, dst, bytes, error)) return false;
        ++files;
        return true;
    }
    if (!S_ISDIR(st.st_mode)) {
        error = "not a regular file or directory: " + src;
        return false;
    }
    if (!ensureDir(dst)) {
        error = "cannot create " + dst;
        return false;
    }
    DIR* d = ::opendir(src.c_str());
    if (!d) {
        error = "cannot read directory " + src;
        return false;
    }
    bool ok = true;
    while (struct dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name.empty() || name == "." || name == ".." || name[0] == '.')
            continue;
        if (files >= kMaxToolFiles) {
            error = "the import has more than " +
                    std::to_string(kMaxToolFiles) + " files";
            ok = false;
            break;
        }
        if (!copyTreeInto(src + "/" + name, dst + "/" + name, bytes, files,
                          error)) {
            ok = false;
            break;
        }
    }
    ::closedir(d);
    return ok;
}

class SelfMadeToolsAction : public action::Action {
  public:
    const char* name() const override { return "self-made-tool"; }

    const char* usage() const override {
        return "self-made-tool(op, name, explanation?, ...) — create, inspect, "
               "edit and run tools of your own under "
               "<config>/self-made-tools/<name>/; keep a tool's files there, "
               "not in the workspace. op=add creates a tool and grants "
               "read/write (approval); op=delete removes the whole tool "
               "(approval), or one file when `path` is given; "
               "op=list [name?] lists tools or their files; op=read, op=grep "
               "need no grant; op=write, op=append, op=edit (with path) and "
               "op=run change or run files inside the tool; op=import copies a "
               "readable file or directory into the tool. The first write, "
               "import or run (and op=edit without a path) asks for approval "
               "and needs an explanation. File paths are relative to the tool "
               "directory; use a subdirectory such as src/ for source files";
    }

    action::Result run(
        const std::map<std::string, std::string>& args) const override {
        const std::string op = opArg(args);
        const std::string name = nameArg(args);

        // Listing all installed tools needs no name.
        if (op == "list") return listTools(name);

        if (op.empty()) {
            return fail("self-made-tool needs an op; use add, edit, delete, "
                        "list, read, write, append, grep, run or import");
        }
        if (name.empty()) return fail("self-made-tool needs a tool name");
        if (!validName(name)) {
            return fail("invalid tool name \"" + name +
                        "\"; use letters, digits, '-', '_' or '.', not starting "
                        "with '.'");
        }

        if (op == "add" || op == "delete") {
            const std::string path = argValue(args, "path");
            if (op == "delete" && !path.empty())
                return deleteToolFile(name, args);
            const std::string explanation = explanationArg(args);
            if (explanation.empty())
                return fail(
                    "self-made-tool needs an explanation to show the user");
            return op == "add" ? add(name, explanation)
                               : remove(name, explanation);
        }

        if (op == "edit") {
            const std::string path = argValue(args, "path");
            if (!path.empty()) return editToolFile(name, args);
            if (!argValue(args, "old").empty() ||
                !argValue(args, "new").empty())
                return fail("op=edit with `old` or `new` needs a `path`");
            const std::string explanation = explanationArg(args);
            if (explanation.empty())
                return fail(
                    "self-made-tool needs an explanation to show the user");
            return edit(name, explanation);
        }

        if (op == "read") return readToolFile(name, args);
        if (op == "write") return writeToolFile(name, args, false);
        if (op == "append") return writeToolFile(name, args, true);
        if (op == "grep") return grepTool(name, args);
        if (op == "run") return runTool(name, args);
        if (op == "import") return importInto(name, args);

        return fail("unknown self-made-tool op \"" + op +
                    "\"; use add, edit, delete, list, read, write, append, "
                    "grep, run or import");
    }

  private:
    bool askPermission(const std::string& op, const std::string& name,
                       const std::string& explanation,
                       action::Result& r) const {
        std::string error;
        if (!requestPermission(op, name, explanation, error)) {
            r.output = failure(error);
            return false;
        }
        return true;
    }

    // Writes, imports and runs need a grant. A tool that was found on disk is
    // readable without one, so prompt here (with the AI's explanation) and
    // record the grant instead of failing and making the loop retry.
    bool ensureGranted(const std::string& name, const std::string& canonicalDir,
                       const std::string& explanation,
                       const std::string& approvalOp, bool& saved,
                       action::Result& r) const {
        saved = true;
        if (!self_made_tools::grantedRoot(canonicalDir).empty()) return true;
        if (explanation.empty()) {
            r.output = failure(
                "tool \"" + name +
                "\" is readable but not granted for writes or runs; include an "
                "explanation so the user can approve it, or call "
                "self-made-tool(op=edit, name=" + name +
                ", explanation=...) first");
            return false;
        }
        if (!askPermission(approvalOp, name, explanation, r)) return false;
        saved = grant(canonicalDir);
        return true;
    }

    action::Result listTools(const std::string& name) const {
        action::Result r;
        const std::string root = self_made_tools::rootPath();

        if (name.empty()) {
            DIR* d = ::opendir(root.c_str());
            if (!d) {
                return fail("there is no self-made-tools directory yet; use "
                            "op=add to create a tool");
            }
            std::vector<std::string> names;
            while (struct dirent* ent = ::readdir(d)) {
                const std::string entry = ent->d_name;
                if (entry.empty() || entry[0] == '.' || !validName(entry))
                    continue;
                const std::string dir = root + "/" + entry;
                struct stat st;
                if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                    continue;
                names.push_back(entry);
            }
            ::closedir(d);
            std::sort(names.begin(), names.end());

            std::ostringstream out;
            out << "tools: " << names.size();
            if (!names.empty()) out << "\n---\n";
            for (const std::string& entry : names) {
                std::vector<FileEntry> files;
                collectFiles(root + "/" + entry, "", files);
                long long bytes = 0;
                for (const FileEntry& file : files) bytes += file.size;
                out << entry << "/  files=" << files.size()
                    << " bytes=" << bytes;
                if (files.size() >= kMaxToolFiles) out << " (listing capped)";
                out << "\n";
            }
            r.ok = true;
            r.output = out.str();
            r.facts.push_back(facts::make(
                facts::FactKind::Listing, "dir:" + root, root,
                "tools: " + std::to_string(names.size()),
                facts::FactStatus::Verified, "readdir"));
            return r;
        }

        if (!validName(name)) {
            return fail("invalid tool name \"" + name + "\"");
        }
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        std::vector<FileEntry> files;
        collectFiles(ref.dir, "", files);
        std::sort(files.begin(), files.end(),
                  [](const FileEntry& a, const FileEntry& b) {
                      return a.rel < b.rel;
                  });
        long long bytes = 0;
        for (const FileEntry& file : files) bytes += file.size;

        std::ostringstream out;
        out << "tool: " << name << "\n"
            << "path: " << ref.dir << "\n"
            << "files: " << files.size() << " (" << bytes << " bytes)";
        if (files.size() >= kMaxToolFiles) out << " (listing capped)";
        if (!files.empty()) out << "\n---\n";
        for (const FileEntry& file : files)
            out << file.rel << "  (" << file.size << " bytes)\n";

        r.ok = true;
        r.output = out.str();
        r.facts.push_back(facts::make(
            facts::FactKind::Listing, "dir:" + ref.dir, ref.dir,
            "files: " + std::to_string(files.size()) + ", bytes: " +
                std::to_string(bytes),
            facts::FactStatus::Verified, "readdir"));
        return r;
    }

    action::Result readToolFile(
        const std::string& name,
        const std::map<std::string, std::string>& args) const {
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        std::string full;
        if (!toolFilePath(ref, argValue(args, "path"), full, error))
            return fail(error);

        action::Request req;
        req.name = "read_file";
        req.args["path"] = full;
        for (const char* key : {"offset", "limit"}) {
            auto it = args.find(key);
            if (it != args.end()) req.args[key] = it->second;
        }
        return action::execute(req);
    }

    action::Result writeToolFile(
        const std::string& name,
        const std::map<std::string, std::string>& args, bool append) const {
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        std::string full;
        if (!toolFilePath(ref, argValue(args, "path"), full, error))
            return fail(error);

        action::Result r;
        bool saved = true;
        if (!ensureGranted(name, ref.dir, explanationArg(args),
                           append ? "append" : "write", saved, r))
            return r;

        action::Request req;
        req.name = append ? "append_file" : "write_file";
        req.args["path"] = full;
        req.args["content"] = argValue(args, "content");
        action::Result result = action::execute(req);
        if (!saved) result.output += manifestWarning();
        return result;
    }

    action::Result editToolFile(
        const std::string& name,
        const std::map<std::string, std::string>& args) const {
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        const std::string oldText = argValue(args, "old");
        if (oldText.empty())
            return fail("op=edit needs a non-empty `old` argument");
        std::string full;
        if (!toolFilePath(ref, argValue(args, "path"), full, error))
            return fail(error);

        action::Result r;
        bool saved = true;
        if (!ensureGranted(name, ref.dir, explanationArg(args), "edit", saved,
                           r))
            return r;

        action::Request req;
        req.name = "edit_file";
        req.args["path"] = full;
        req.args["old"] = oldText;
        req.args["new"] = argValue(args, "new");
        action::Result result = action::execute(req);
        if (!saved) result.output += manifestWarning();
        return result;
    }

    action::Result deleteToolFile(
        const std::string& name,
        const std::map<std::string, std::string>& args) const {
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        std::string full;
        if (!toolFilePath(ref, argValue(args, "path"), full, error))
            return fail(error);

        action::Result r;
        bool saved = true;
        if (!ensureGranted(name, ref.dir, explanationArg(args), "delete", saved,
                           r))
            return r;

        struct stat st;
        if (::lstat(full.c_str(), &st) != 0)
            return fail("no such file in tool \"" + name + "\": " +
                        argValue(args, "path"));
        if (S_ISLNK(st.st_mode))
            return fail("refusing to delete the symbolic link " + full);
        if (S_ISDIR(st.st_mode))
            return fail("path names a directory; remove the whole tool or "
                        "delete the files one by one");
        if (!S_ISREG(st.st_mode))
            return fail("not a regular file: " + full);

        if (::unlink(full.c_str()) != 0)
            return fail("could not delete " + full);

        r.ok = true;
        r.output = "deleted: " + full;
        if (!saved) r.output += manifestWarning();
        r.facts.push_back(facts::make(
            facts::FactKind::Wrote, "file:" + full, full, "deleted",
            facts::FactStatus::Verified, "stat"));
        return r;
    }

    action::Result grepTool(
        const std::string& name,
        const std::map<std::string, std::string>& args) const {
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        const std::string pattern = argValue(args, "pattern");
        if (pattern.empty())
            return fail("op=grep needs a non-empty `pattern` argument");

        long long limit = 50;
        {
            char* end = nullptr;
            const std::string raw = argValue(args, "limit");
            if (!raw.empty()) {
                errno = 0;
                const long long n = std::strtoll(raw.c_str(), &end, 10);
                if (errno == 0 && end != raw.c_str() && n > 0) limit = n;
            }
        }
        if (limit > 500) limit = 500;

        const std::string rel = argValue(args, "path");
        if (!rel.empty()) {
            std::string full;
            if (!toolFilePath(ref, rel, full, error)) return fail(error);
            action::Request req;
            req.name = "grep_file";
            req.args["path"] = full;
            req.args["pattern"] = pattern;
            req.args["limit"] = std::to_string(limit);
            return action::execute(req);
        }

        std::vector<FileEntry> files;
        collectFiles(ref.dir, "", files);
        std::sort(files.begin(), files.end(),
                  [](const FileEntry& a, const FileEntry& b) {
                      return a.rel < b.rel;
                  });

        size_t total = 0;
        size_t shown = 0;
        size_t skipped = 0;
        std::ostringstream matches;
        for (const FileEntry& file : files) {
            std::string data;
            std::string readError;
            if (!readTextFile(ref.dir + "/" + file.rel, kMaxToolFileBytes, data,
                              readError)) {
                ++skipped;
                continue;
            }
            const text_grep::Result found =
                text_grep::find(data, pattern, 0);
            if (!found.ok) return fail(found.error);
            total += found.total;
            for (const text_grep::Match& m : found.matches) {
                if (shown >= static_cast<size_t>(limit)) break;
                matches << file.rel << ":" << m.line << ":" << m.offset << ":"
                        << m.text << "\n";
                ++shown;
            }
        }

        std::ostringstream out;
        out << "tool: " << name << "\n"
            << "path: " << ref.dir << "\n"
            << "pattern: " << pattern << "\n"
            << "matches: " << total;
        if (total > shown) out << " (showing " << shown << ")";
        if (skipped) out << "; skipped " << skipped << " binary or large file(s)";
        out << "\n---\n" << matches.str();
        if (total == 0) out << "no match\n";

        action::Result r;
        r.ok = true;
        r.output = out.str();
        std::string value = "no match";
        if (total > 0) value = std::to_string(total) + " match" +
                               (total == 1 ? "" : "es");
        r.facts.push_back(facts::make(
            facts::FactKind::Generic,
            "grep-tool:" + name + ":" + pattern, ref.dir, value,
            facts::FactStatus::Verified, "content"));
        return r;
    }

    action::Result runTool(
        const std::string& name,
        const std::map<std::string, std::string>& args) const {
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        std::string full;
        if (!toolFilePath(ref, argValue(args, "path"), full, error))
            return fail(error);

        action::Result r;
        bool saved = true;
        if (!ensureGranted(name, ref.dir, explanationArg(args), "run", saved,
                           r))
            return r;

        action::Request req;
        req.name = "sandbox-run-script";
        req.args["path"] = full;
        for (const char* key : {"args", "timeout_sec"}) {
            auto it = args.find(key);
            if (it != args.end()) req.args[key] = it->second;
        }
        action::Result result = action::execute(req);
        if (!saved) result.output += manifestWarning();
        return result;
    }

    action::Result importInto(
        const std::string& name,
        const std::map<std::string, std::string>& args) const {
        ToolRef ref;
        std::string error;
        if (!resolveTool(name, ref, error)) return fail(error);

        const std::string source = trim(argValue(args, "source"));
        if (source.empty())
            return fail("op=import needs a `source` path");
        const std::string src = action::resolveReadablePath(source);
        if (src.empty()) {
            const std::string why = action::readablePathProblem(source);
            return fail(why.empty() ? "source is not readable: " + source
                                    : why);
        }

        struct stat srcSt;
        if (::lstat(src.c_str(), &srcSt) != 0)
            return fail("cannot inspect " + src);
        if (S_ISLNK(srcSt.st_mode))
            return fail("refusing to import the symbolic link " + src);
        if (!S_ISREG(srcSt.st_mode) && !S_ISDIR(srcSt.st_mode))
            return fail("import source is not a regular file or directory: " +
                        src);

        std::string destRel = argValue(args, "path");
        if (destRel.empty()) destRel = leafName(src);
        std::string dest;
        if (!toolFilePath(ref, destRel, dest, error)) return fail(error);

        const std::string canonSrc = canonicalPath(src);
        const std::string canonDest = canonicalPath(dest);
        if (!canonSrc.empty() && !canonDest.empty() && canonSrc == canonDest)
            return fail("the source and destination are the same file");
        if (S_ISDIR(srcSt.st_mode) && !canonSrc.empty() &&
            inside(canonSrc, dest))
            return fail("cannot import a directory into itself");

        action::Result r;
        bool saved = true;
        if (!ensureGranted(name, ref.dir, explanationArg(args), "import", saved,
                           r))
            return r;

        if (!checkParentComponents(ref.dir, destRel, error))
            return fail(error);
        if (!ensureDir(parentOf(dest)))
            return fail("cannot create the destination directory for " + dest);
        const std::string canonicalParent = canonicalPath(parentOf(dest));
        if (canonicalParent.empty() || !inside(ref.dir, canonicalParent))
            return fail("the destination escapes the tool directory");

        long long bytes = 0;
        size_t files = 0;
        if (!copyTreeInto(src, dest, bytes, files, error))
            return fail(error);

        std::ostringstream out;
        out << "imported: " << src << " -> " << dest << "\n"
            << "files: " << files << "\n"
            << "bytes: " << bytes << "\n"
            << "source: " << source;
        if (!saved) out << manifestWarning();

        r.ok = true;
        r.output = out.str();
        r.facts.push_back(facts::make(
            facts::FactKind::Wrote, "file:" + dest, dest,
            "imported " + std::to_string(files) + " file(s), " +
                std::to_string(bytes) + " bytes",
            facts::FactStatus::Reported, "stat"));
        return r;
    }

    action::Result add(const std::string& name,
                       const std::string& explanation) const {
        action::Result r;
        const std::string root = self_made_tools::rootPath();
        const std::string dir = toolDir(name);

        // Re-adding an installed tool that already has a grant changes
        // nothing. Resolve that case before asking the user: a repeated add
        // must not turn into another approval prompt for a no-op, which is
        // what made the loop nag on every retry.
        std::string canonicalRoot = canonicalPath(root);
        std::string canonicalDir = canonicalPath(dir);
        if (!canonicalDir.empty()) {
            struct stat st;
            if (::stat(canonicalDir.c_str(), &st) != 0 ||
                !S_ISDIR(st.st_mode)) {
                r.output = failure("a non-directory entry already exists at " +
                                   dir);
                return r;
            }
            if (canonicalRoot.empty() || !inside(canonicalRoot, canonicalDir)) {
                r.output = failure("the tool directory escapes " + root);
                return r;
            }
            if (!self_made_tools::grantedRoot(canonicalDir).empty()) {
                const bool saved = grant(canonicalDir);
                r.ok = true;
                r.output = "self-made tool \"" + name +
                           "\" is already installed at " + canonicalDir +
                           "\nread/write already enabled; keep its files in "
                           "that folder. Use self-made-tool(op=write, "
                           "name=" + name + ", path=..., content=...), "
                           "op=import to copy a draft in, or op=grep/read/run";
                if (!saved) r.output += manifestWarning();
                return r;
            }
        }

        if (!askPermission("add", name, explanation, r)) return r;

        if (!ensureDir(root)) {
            r.output = failure("cannot create the self-made-tools directory");
            return r;
        }

        struct stat st;
        if (::lstat(dir.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                r.output = failure("a non-directory entry already exists at " +
                                   dir);
                return r;
            }
        } else if (errno != ENOENT) {
            r.output = failure("cannot inspect " + dir);
            return r;
        } else if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            r.output = failure("cannot create " + dir);
            return r;
        }

        canonicalRoot = canonicalPath(root);
        canonicalDir = canonicalPath(dir);
        if (canonicalRoot.empty() || canonicalDir.empty() ||
            !inside(canonicalRoot, canonicalDir)) {
            r.output = failure("the tool directory escapes " + root);
            return r;
        }

        const bool saved = grant(canonicalDir);
        r.facts.push_back(facts::make(
            facts::FactKind::Tool, "tool:" + name, canonicalDir,
            "installed at " + canonicalDir, facts::FactStatus::Reported,
            "stat"));
        r.ok = true;
        r.output = "self-made tool \"" + name + "\" is available at " +
                   canonicalDir +
                   "\nread/write enabled and recorded for later sessions; keep "
                   "its files in that folder. Use self-made-tool(op=write, "
                   "name=" + name + ", path=..., content=...), op=import to "
                   "copy a draft in, or op=grep/read/run";
        if (!saved) r.output += manifestWarning();
        return r;
    }

    action::Result edit(const std::string& name,
                        const std::string& explanation) const {
        action::Result r;
        const std::string dir = toolDir(name);
        const std::string canonicalDir = canonicalPath(dir);
        if (canonicalDir.empty()) {
            r.output = failure(unavailable(name));
            return r;
        }

        struct stat st;
        if (::stat(canonicalDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            r.output = failure(unavailable(name));
            return r;
        }

        const std::string canonicalRoot =
            canonicalPath(self_made_tools::rootPath());
        if (canonicalRoot.empty() || !inside(canonicalRoot, canonicalDir)) {
            r.output = failure("\"" + name +
                               "\" is not inside the self-made-tools directory");
            return r;
        }

        if (self_made_tools::grantedRoot(canonicalDir).empty()) {
            if (!askPermission("edit", name, explanation, r)) return r;
        }

        const bool saved = grant(canonicalDir);
        r.facts.push_back(facts::make(
            facts::FactKind::Tool, "tool:" + name, canonicalDir,
            "found at " + canonicalDir, facts::FactStatus::Reported, "stat"));
        r.ok = true;
        r.output = "found self-made tool \"" + name + "\" at " + canonicalDir +
                   "\nread/write enabled and recorded for later sessions; keep "
                   "its files in that folder. Use self-made-tool(op=write, "
                   "name=" + name + ", path=..., content=...), op=import, or "
                   "op=grep/read/run inside it";
        if (!saved) r.output += manifestWarning();
        return r;
    }

    action::Result remove(const std::string& name,
                          const std::string& explanation) const {
        action::Result r;
        const std::string dir = toolDir(name);
        struct stat st;
        if (::lstat(dir.c_str(), &st) != 0) {
            r.output = failure("there is no such self-made tool \"" + name +
                               "\"");
            return r;
        }
        if (!S_ISDIR(st.st_mode)) {
            r.output =
                failure("\"" + name + "\" is not a self-made tool directory");
            return r;
        }

        const std::string canonicalRoot =
            canonicalPath(self_made_tools::rootPath());
        const std::string canonicalDir = canonicalPath(dir);
        if (canonicalRoot.empty() || canonicalDir.empty() ||
            !inside(canonicalRoot, canonicalDir)) {
            r.output = failure("the tool directory escapes " +
                               self_made_tools::rootPath());
            return r;
        }

        if (!askPermission("delete", name, explanation, r)) return r;

        if (::nftw(canonicalDir.c_str(), removeEntry, 16,
                   FTW_DEPTH | FTW_PHYS) != 0) {
            r.output = failure("could not completely remove " + canonicalDir +
                               "; some files may remain");
            return r;
        }

        const bool saved = revoke(canonicalDir);
        r.ok = true;
        r.output = "removed self-made tool \"" + name + "\" from " +
                   canonicalDir;
        if (!saved) r.output += manifestWarning();
        return r;
    }
};

SelfMadeToolsAction g_action;

} // namespace

namespace self_made_tools {

const action::Action* tool() { return &g_action; }

std::string rootPath() {
    return configSubpath("self-made-tools");
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

void loadGrants() {
    const std::string root = canonicalPath(rootPath());
    if (root.empty()) return;

    const std::string manifest = root + "/.grants";
    std::ifstream in(manifest);
    const bool haveManifest = in.good();

    std::vector<std::string> restored;
    std::string line;
    while (haveManifest && std::getline(in, line)) {
        const std::string name = trim(line);
        // Only plain tool folders may be restored; a hand-edited manifest
        // cannot name a parent path or another directory.
        if (!validName(name)) continue;
        const std::string dir = canonicalPath(root + "/" + name);
        if (dir.empty() || !inside(root, dir)) continue;
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        restored.push_back(dir);
    }

    if (!haveManifest) {
        // Migration for installations created before the manifest existed:
        // a folder under self-made-tools/ was itself approved by add/edit.
        // Everything else in that directory (the dot-files) is ignored.
        DIR* dir = ::opendir(root.c_str());
        if (!dir) return;
        while (struct dirent* ent = ::readdir(dir)) {
            const std::string name = ent->d_name;
            if (name.empty() || name[0] == '.' || !validName(name)) continue;
            const std::string tool = canonicalPath(root + "/" + name);
            if (tool.empty() || !inside(root, tool)) continue;
            struct stat st;
            if (::stat(tool.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            restored.push_back(tool);
        }
        ::closedir(dir);
    }

    std::lock_guard<std::mutex> lock(g.mutex);
    for (const std::string& dir : restored) {
        if (std::find(g.roots.begin(), g.roots.end(), dir) == g.roots.end())
            g.roots.push_back(dir);
    }
    if (!haveManifest && !restored.empty()) writeGrantsLocked();
}

Pending pending() {
    std::lock_guard<std::mutex> lock(p.mutex);
    Pending out;
    out.active = p.pending;
    if (out.active) {
        out.op = p.op;
        out.name = p.name;
        out.explanation = p.explanation;
        out.id = p.pendingId;
    }
    return out;
}

bool hasPending() {
    std::lock_guard<std::mutex> lock(p.mutex);
    return p.pending;
}

bool decide(bool allow) {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.pending) return false;
    p.pending = false;
    p.hasDecision = true;
    p.allow = allow;
    p.reason.clear();
    p.cv.notify_all();
    return true;
}

void cancel(const std::string& reason) {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.pending) return;
    p.pending = false;
    p.hasDecision = true;
    p.allow = false;
    p.reason = reason.empty() ? "the self-made tool request was cancelled"
                              : reason;
    p.cv.notify_all();
}

} // namespace self_made_tools
