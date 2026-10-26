#include "aichat/core/experience.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/facts/text.h"
#include "aichat/core/util.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace {

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
        const std::string part = dir.substr(i, j - i);
        if (!part.empty()) {
            if (!acc.empty() && acc.back() != '/') acc += '/';
            acc += part;
            if (::mkdir(acc.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
        if (j == dir.size()) break;
        i = j + 1;
    }
    return isDirectory(dir);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) {
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

std::string targetLine(const std::string& desc) {
    std::istringstream in(desc);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("target:", 0) == 0)
            return trim(line.substr(7));
        if (!line.empty() && line[0] != '#') break;
    }
    return "";
}

std::string fnv8(const std::string& text) {
    unsigned int hash = 2166136261u;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", hash);
    return buf;
}

std::string slugify(const std::string& target) {
    std::string out;
    bool dash = false;
    for (unsigned char c : target) {
        if (c >= 0x80) {
            out += static_cast<char>(c);
            dash = false;
        } else if (std::isalnum(c)) {
            out += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            dash = false;
        } else if (!out.empty() && !dash) {
            out += '-';
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    out = text_utf8::head(out, 80);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "target" : out;
}

std::string dirFor(const std::string& id) {
    return experience::rootPath() + "/" + id;
}

std::string descFor(const std::string& id) {
    return dirFor(id) + "/" + id + ".desc";
}

std::string expFor(const std::string& id) {
    return dirFor(id) + "/" + id + ".exp";
}

std::string stepsFor(const std::string& id) {
    return dirFor(id) + "/" + id + ".steps";
}

std::string threadIdFromDesc(const std::string& desc) {
    std::istringstream in(desc);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("thread:", 0) == 0)
            return slugify(trim(line.substr(7)));
    }
    return "";
}

std::string threadNameFor(const std::string& id) {
    const std::string thread = threadIdFromDesc(readFile(descFor(id)));
    return thread.empty() ? id : thread;
}

std::string threadRootFor(const std::string& id) {
    const std::string thread = threadIdFromDesc(readFile(descFor(id)));
    if (thread.empty()) return dirFor(id);
    return experience::rootPath() + "/.threads/" + thread;
}

std::string stateForTarget(const std::string& id) {
    const std::string name = threadNameFor(id);
    return threadRootFor(id) + "/" + name + ".state";
}

std::string journalForTarget(const std::string& id) {
    const std::string name = threadNameFor(id);
    return threadRootFor(id) + "/" + name + ".journal";
}

std::string practiceDigestPath() {
    return experience::rootPath() + "/practice.exp";
}

std::string timestamp();

// Append one timestamped block. The header is written once per file. Bodies
// are sanitized and capped so a runaway model cannot fill the disk.
bool appendEntry(const std::string& path, const std::string& target,
                 const std::string& headerLabel, const std::string& title,
                 const std::string& body, size_t cap) {
    const bool fresh = ::access(path.c_str(), F_OK) != 0;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    if (fresh) {
        out << "# " << headerLabel << ": " << target << "\n"
            << "# file: " << path.substr(path.find_last_of('/') + 1) << "\n";
    }
    const std::string clean = text_utf8::sanitize(body);
    out << "\n== " << timestamp() << " | "
        << (trim(title).empty() ? "note" : trim(title)) << " ==\n"
        << text_utf8::head(clean, cap);
    if (!clean.empty() && clean.back() != '\n') out << "\n";
    out.close();
    if (!out) return false;
    ::chmod(path.c_str(), 0600);
    return true;
}

std::string collapseLine(const std::string& text, size_t cap) {
    std::string out;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (!out.empty() && out.back() == ' ') continue;
            out += ' ';
        } else {
            out += c;
        }
    }
    out = trim(out);
    while (out.find("  ") != std::string::npos)
        out.replace(out.find("  "), 2, " ");
    return text_utf8::head(out, cap);
}

std::string timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

} // namespace

namespace experience {

std::string rootPath() { return configSubpath("ai-experiences"); }

std::string targetId(const std::string& target) {
    const std::string base = slugify(target);
    auto state = [&](const std::string& id) {
        const std::string desc = descFor(id);
        std::ifstream in(desc);
        if (!in) return 0;  // free
        return targetLine(readFile(desc)) == target ? 1 : -1;
    };

    int s = state(base);
    if (s >= 0) return base;

    const std::string hashed = base + "-" + fnv8(target);
    s = state(hashed);
    if (s >= 0) return hashed;

    // A hash collision on different targets is effectively impossible, but
    // keep the layout deterministic if it ever happens.
    return hashed + "-2";
}

std::string begin(const std::string& target, const std::string& description,
                  const std::string& thread) {
    const std::string id = targetId(target);
    if (!ensureDir(dirFor(id))) return "";
    const std::string path = descFor(id);
    if (::access(path.c_str(), F_OK) == 0) return id;

    std::ostringstream out;
    out << "target: " << target << "\n";
    const std::string threadSlug = thread.empty() ? "" : slugify(thread);
    if (!threadSlug.empty()) out << "thread: " << threadSlug << "\n";
    out << "description:\n"
        << (trim(description).empty() ? "(no description recorded)"
                                      : trim(description))
        << "\n";
    if (!writeFile(path, out.str())) return "";
    return id;
}

std::string record(const std::string& target, const std::string& title,
                   const std::string& body) {
    const std::string id = begin(target, "");
    if (id.empty()) return "";
    const std::string path = expFor(id);
    if (!appendEntry(path, target, "experience", title, body, 6000))
        return "";
    return path;
}

std::string recordStep(const std::string& target, const std::string& title,
                       const std::string& body) {
    const std::string id = begin(target, "");
    if (id.empty()) return "";
    const std::string path = stepsFor(id);
    if (!appendEntry(path, target, "steps", title, body, 6000))
        return "";
    return path;
}

std::string writeState(const std::string& target, const std::string& body) {
    std::string clean = trim(body);
    if (clean.empty()) return "";
    // Models sometimes echo the app's header back. Strip a leading header so
    // the state file does not accumulate one copy per rewrite.
    for (;;) {
        const size_t start = clean.find_first_not_of("\r\n");
        if (start == std::string::npos) return "";
        clean.erase(0, start);
        if (clean.rfind("# state:", 0) != 0 &&
            clean.rfind("# updated:", 0) != 0)
            break;
        const size_t nl = clean.find('\n');
        if (nl == std::string::npos) return "";
        clean.erase(0, nl + 1);
    }
    const std::string id = begin(target, "");
    if (id.empty()) return "";
    const std::string name = threadNameFor(id);
    const std::string root = threadRootFor(id);
    if (!ensureDir(root)) return "";
    const std::string path = root + "/" + name + ".state";

    // Keep the previous state before replacing the only copy. If the backup
    // cannot be written, leave the current state untouched.
    const std::string current = readFile(path);
    if (!trim(current).empty()) {
        if (!writeFile(path + ".prev", current)) return "";
    }

    std::ostringstream out;
    out << "# state: " << name << "\n"
        << "# updated: " << timestamp() << "\n\n"
        << text_utf8::head(text_utf8::sanitize(clean), 4000);
    std::string content = out.str();
    if (content.empty() || content.back() != '\n') content += '\n';
    if (!writeFile(path, content)) return "";
    return path;
}

std::string loadState(const std::string& target) {
    return readFile(stateForTarget(targetId(target)));
}

std::string recordJournal(const std::string& target, const std::string& body) {
    if (trim(body).empty()) return "";
    const std::string id = begin(target, "");
    if (id.empty()) return "";
    const std::string name = threadNameFor(id);
    const std::string root = threadRootFor(id);
    if (!ensureDir(root)) return "";
    const std::string path = root + "/" + name + ".journal";
    if (!appendEntry(path, name, "reader journal", "entry", body, 8000))
        return "";
    return path;
}

std::string recordLesson(const std::string& target, const std::string& body) {
    if (trim(body).empty()) return "";
    const std::string id = begin(target, "");
    if (id.empty()) return "";
    const std::string path = expFor(id);
    if (!appendEntry(path, target, "experience", "lesson", body, 3000))
        return "";

    // A bounded cross-target index: one collapsed line per lesson, so a
    // different target can see that a reusable rule exists without loading
    // every experience directory.
    const std::string digest = practiceDigestPath();
    const bool fresh = ::access(digest.c_str(), F_OK) != 0;
    std::ofstream out(digest, std::ios::binary | std::ios::app);
    if (out) {
        if (fresh)
            out << "# practice index: one line per recorded lesson\n";
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        char date[16];
        std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
        out << "- " << date << " | " << id << " | "
            << collapseLine(body, 320) << "\n";
        out.close();
        ::chmod(digest.c_str(), 0600);

        // Keep the index bounded: it is a pointer list, not an archive.
        struct stat st;
        if (::stat(digest.c_str(), &st) == 0 && st.st_size > 64 * 1024) {
            std::istringstream in(readFile(digest));
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(in, line)) lines.push_back(line);
            const size_t keep = 200;
            std::ostringstream trimmed;
            trimmed << "# practice index: one line per recorded lesson\n";
            const size_t start = lines.size() > keep ? lines.size() - keep : 0;
            for (size_t i = start; i < lines.size(); ++i) {
                if (lines[i].empty() || lines[i][0] == '#') continue;
                trimmed << lines[i] << "\n";
            }
            writeFile(digest, trimmed.str());
        }
    }
    return path;
}

std::string load(const std::string& target) {
    const std::string id = targetId(target);
    const std::string desc = readFile(descFor(id));
    const std::string exp = readFile(expFor(id));
    const std::string state = readFile(stateForTarget(id));
    const std::string journal = readFile(journalForTarget(id));
    const std::string thread = threadNameFor(id);
    const std::string digest = readFile(practiceDigestPath());
    if (desc.empty() && exp.empty() && state.empty() && journal.empty() &&
        digest.empty())
        return "";

    std::ostringstream out;
    out << "Experience directory: ai-experiences/" << id << "/\n";
    if (thread != id) out << "Thread: " << thread << "\n";
    out << "Layer reminder: .desc is the detailed account of this target; "
           ".exp is distilled practice (purpose -> practice -> evidence); "
           ".state is the thread's current state (frontier, coverage, open "
           "gaps, next); .journal is the thread's first-person record (what "
           "you noticed, thought, liked or disliked, and want next); .steps "
           "is the operational ledger and is not loaded here. Use .state for "
           "where the work stands and .journal for your own voice. Reuse or "
           "extend the practice; do not copy the ledger. A block marked "
           "\"compatible legacy - worthy reference for how-to\" is an older "
           "skill record: use it as a how-to reference, not as the current "
           "lesson format.\n";
    if (!desc.empty())
        out << "\nDescription (.desc):\n"
            << text_utf8::tail(desc, 3000) << "\n";
    if (!exp.empty())
        out << "\nPractice for this target (.exp, latest last):\n"
            << text_utf8::tailLines(exp, 6000) << "\n";
    if (!state.empty())
        out << "\nCurrent state (.state):\n"
            << text_utf8::tail(state, 3000) << "\n";
    if (!journal.empty())
        out << "\nReader journal (.journal, latest last):\n"
            << text_utf8::tail(journal, 3000) << "\n";
    if (::access(stepsFor(id).c_str(), F_OK) == 0)
        out << "\nOperational ledger present: ai-experiences/" << id << "/"
            << id << ".steps (not loaded; current task state is the fact "
               "store).\n";

    // Cross-target digest, minus this target's own lines (already above).
    if (!digest.empty()) {
        std::istringstream in(digest);
        std::string line;
        std::ostringstream picked;
        const std::string self = "| " + id + " |";
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line.find(self) != std::string::npos) continue;
            picked << line << "\n";
        }
        std::string text = picked.str();
        if (!trim(text).empty())
            out << "\nPractice from other targets (one line each; full text "
                   "is in that target's .exp):\n"
                << text_utf8::tail(text, 1600) << "\n";
    }
    return out.str();
}


std::string stepsPath(const std::string& target) {
    const std::string id = targetId(target);
    return id.empty() ? std::string() : stepsFor(id);
}

std::string statePath(const std::string& target) {
    const std::string id = targetId(target);
    return id.empty() ? std::string() : stateForTarget(id);
}

std::string journalPath(const std::string& target) {
    const std::string id = targetId(target);
    return id.empty() ? std::string() : journalForTarget(id);
}

std::string practicePath() { return practiceDigestPath(); }

std::string pendingPath() {
    return configSubpath("pending-experience.mem");
}

namespace {

constexpr size_t kMaxPending = 200;

std::vector<std::string> splitTabs(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t p = s.find('\t', start);
        if (p == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return parts;
}

std::vector<PendingRecord> loadPendingFile() {
    std::vector<PendingRecord> out;
    std::ifstream in(pendingPath());
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> parts = splitTabs(line);
        if (parts.size() < 3) continue;
        PendingRecord record;
        record.target = facts::unescapeField(parts[0]);
        record.title = facts::unescapeField(parts[1]);
        record.body = facts::unescapeField(parts[2]);
        record.channel = parts.size() >= 4
                             ? facts::unescapeField(parts[3])
                             : std::string("step");
        if (record.channel != "practice" && record.channel != "journal")
            record.channel = "step";
        if (record.target.empty() || record.body.empty()) continue;
        out.push_back(std::move(record));
    }
    return out;
}

bool savePendingFile(const std::vector<PendingRecord>& pending) {
    const std::string path = pendingPath();
    if (path.empty()) return false;
    if (pending.empty()) {
        std::remove(path.c_str());
        return true;
    }
    ensureConfigDir();
    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << "# ai-chat pending experience v1\n";
    for (const PendingRecord& record : pending) {
        out << facts::escapeField(record.target) << '\t'
            << facts::escapeField(record.title) << '\t'
            << facts::escapeField(record.body) << '\t'
            << facts::escapeField(record.channel.empty() ? "step"
                                                         : record.channel)
            << '\n';
    }
    out.close();
    if (!out) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

namespace {

size_t queueChannel(const std::string& target, const std::string& title,
                    const std::string& body, bool quiet,
                    const std::string& channel) {
    if (trim(body).empty()) return pendingCount();
    const std::string kind =
        (channel == "practice" || channel == "journal") ? channel
                                                        : std::string("step");
    bool wrote = false;
    if (kind == "practice")
        wrote = !recordLesson(target, body).empty();
    else if (kind == "journal")
        wrote = !recordJournal(target, body).empty();
    else
        wrote = !recordStep(target, title, body).empty();
    if (wrote) return flushPending();

    std::vector<PendingRecord> pending = loadPendingFile();
    pending.push_back({target, title, body, kind});
    if (pending.size() > kMaxPending) {
        pending.erase(pending.begin());
        if (!quiet)
            std::fprintf(stderr, "ai-chat: pending experience queue full; "
                                 "dropped the oldest record\n");
    }
    if (!savePendingFile(pending)) {
        if (!quiet)
            std::fprintf(stderr,
                         "ai-chat: cannot save the pending experience "
                         "queue\n");
    }
    return pending.size();
}

} // namespace

std::vector<PendingRecord> loadPending() { return loadPendingFile(); }

size_t queue(const std::string& target, const std::string& title,
             const std::string& body, bool quiet) {
    return queueChannel(target, title, body, quiet, "step");
}

size_t queueLesson(const std::string& target, const std::string& title,
                   const std::string& body, bool quiet) {
    return queueChannel(target, title, body, quiet, "practice");
}

size_t queueJournal(const std::string& target, const std::string& title,
                    const std::string& body, bool quiet) {
    return queueChannel(target, title, body, quiet, "journal");
}

size_t flushPending() {
    std::vector<PendingRecord> pending = loadPendingFile();
    if (pending.empty()) return 0;
    std::vector<PendingRecord> keep;
    keep.reserve(pending.size());
    for (const PendingRecord& entry : pending) {
        bool wrote = false;
        if (entry.channel == "practice")
            wrote = !recordLesson(entry.target, entry.body).empty();
        else if (entry.channel == "journal")
            wrote = !recordJournal(entry.target, entry.body).empty();
        else
            wrote = !recordStep(entry.target, entry.title, entry.body).empty();
        if (!wrote) keep.push_back(entry);
    }
    savePendingFile(keep);
    return keep.size();
}

size_t pendingCount() { return loadPendingFile().size(); }

bool migrate(std::string& report, bool dryRun) {
    report.clear();
    const std::string root = rootPath();
    DIR* d = ::opendir(root.c_str());
    if (!d) {
        report = "no experience directory at " + root + "\n";
        return true;
    }

    std::vector<std::string> ids;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(d)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.') continue;
        if (isDirectory(root + "/" + name)) ids.push_back(name);
    }
    ::closedir(d);
    std::sort(ids.begin(), ids.end());

    struct Block {
        std::string header;
        std::string body;
        bool practice = false;
    };
    struct IndexLine {
        std::string date;
        std::string id;
        std::string body;
    };

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    const std::string backupDir =
        root + "/.migration-backup-" + std::string(stamp);

    std::ostringstream lines;
    std::vector<IndexLine> index;
    size_t movedTotal = 0;
    size_t keptTotal = 0;
    size_t markedTotal = 0;
    size_t changedTargets = 0;
    bool ok = true;

    for (const std::string& id : ids) {
        const std::string expPath = expFor(id);
        const std::string text = readFile(expPath);
        if (text.empty()) continue;

        std::vector<Block> blocks;
        Block current;
        bool inBlock = false;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            const bool isHeader =
                line.size() >= 4 && line.rfind("== ", 0) == 0 &&
                line.compare(line.size() - 3, 3, " ==") == 0;
            if (isHeader) {
                if (inBlock) blocks.push_back(current);
                current = Block();
                current.header = line;
                inBlock = true;
            } else if (inBlock) {
                current.body += line;
                current.body += "\n";
            }
        }
        if (inBlock) blocks.push_back(current);
        if (blocks.empty()) continue;

        std::vector<Block> practiceBlocks;
        std::string legacy;
        size_t moved = 0;
        size_t marked = 0;
        for (Block& block : blocks) {
            const bool labelled =
                block.body.find("PURPOSE:") != std::string::npos &&
                block.body.find("PRACTICE:") != std::string::npos;
            // A legacy block with both a solution and its evidence is a
            // usable skill record, not a diary. Keep it, but label it
            // honestly: compatible with the current reader, worth keeping for
            // how-to, not written in the new purpose/practice/evidence form.
            const bool legacySkill =
                block.body.find("solution:") != std::string::npos &&
                block.body.find("evidence:") != std::string::npos;
            block.practice = labelled || legacySkill;
            std::string digestBody = block.body;
            if (legacySkill && !labelled &&
                block.body.find("compatible legacy") == std::string::npos) {
                const std::string note =
                    "status: compatible legacy - worthy reference for how-to; "
                    "use as reference, not as the current lesson format.\n";
                block.body = note + block.body;
                digestBody = block.body;
                ++marked;
            }
            if (block.practice) {
                practiceBlocks.push_back(block);
                const std::string date =
                    block.header.size() >= 13 ? block.header.substr(3, 10)
                                              : std::string();
                index.push_back({date, id, digestBody});
            } else {
                ++moved;
                legacy += "\n" + block.header + "\n" + block.body;
            }
        }
        const size_t kept = practiceBlocks.size();
        keptTotal += kept;
        markedTotal += marked;
        if (moved == 0 && marked == 0) continue;

        ++changedTargets;
        movedTotal += moved;
        lines << id << ": moved " << moved << " to .steps, kept " << kept
              << " practice, marked " << marked << " legacy how-to\n";
        if (dryRun) continue;

        if (!ensureDir(backupDir) ||
            !writeFile(backupDir + "/" + id + ".exp", text)) {
            lines << "  FAILED: could not back up " << id << ".exp\n";
            ok = false;
            continue;
        }

        std::string target = targetLine(readFile(descFor(id)));
        if (target.empty()) target = id;

        if (moved > 0) {
            const std::string steps = stepsFor(id);
            const bool fresh = ::access(steps.c_str(), F_OK) != 0;
            std::ofstream out(steps, std::ios::binary | std::ios::app);
            if (!out) {
                lines << "  FAILED: could not open " << id << ".steps\n";
                ok = false;
                continue;
            }
            if (fresh)
                out << "# steps: " << target << "\n# file: " << id
                    << ".steps\n";
            out << legacy;
            out.close();
            if (!out) {
                lines << "  FAILED: could not write " << id << ".steps\n";
                ok = false;
                continue;
            }
            ::chmod(steps.c_str(), 0600);
        }

        if (practiceBlocks.empty()) {
            if (::unlink(expPath.c_str()) != 0 && errno != ENOENT) {
                lines << "  FAILED: could not remove empty " << id << ".exp\n";
                ok = false;
            }
        } else {
            std::ostringstream kept;
            kept << "# experience: " << target << "\n# file: " << id
                 << ".exp\n";
            for (const Block& block : practiceBlocks)
                kept << "\n" << block.header << "\n" << block.body;
            if (!writeFile(expPath, kept.str())) {
                lines << "  FAILED: could not rewrite " << id << ".exp\n";
                ok = false;
            }
        }
    }

    if (!dryRun) {
        std::ostringstream rebuilt;
        rebuilt << "# practice index: one line per recorded lesson\n";
        for (const IndexLine& entry : index)
            rebuilt << "- " << entry.date << " | " << entry.id << " | "
                    << collapseLine(entry.body, 320) << "\n";
        if (!writeFile(practicePath(), rebuilt.str())) {
            lines << "FAILED: could not rebuild practice.exp\n";
            ok = false;
        }
    }

    if (changedTargets == 0) {
        report = "nothing to migrate: no legacy blocks found in " +
                 std::to_string(ids.size()) + " target director" +
                 (ids.size() == 1 ? "y\n" : "ies\n");
        return ok;
    }
    report = (dryRun ? "dry run: " : "migrated: ") +
             std::to_string(movedTotal) + " legacy block(s) to .steps, " +
             std::to_string(keptTotal) + " practice block(s) kept, " +
             std::to_string(markedTotal) +
             " legacy how-to block(s) marked, " +
             std::to_string(changedTargets) + " target(s) changed\n" +
             lines.str();
    if (!dryRun && ok)
        report += "backup: " + backupDir + "\n";
    return ok;
}

} // namespace experience
