#include "aichat/core/story.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aichat/core/config.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace {

constexpr long long kDay = 24 * 60 * 60;

struct IndexEntry {
    std::string id;
    int score = 0;
    long long last = 0;
    bool open = false;
    bool active = true;
};

std::string slugify(const std::string& text) {
    std::string out;
    bool dash = false;
    for (unsigned char c : text) {
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
    return out;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool ensureDir(const std::string& path) {
    if (::mkdir(path.c_str(), 0700) == 0) return true;
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
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

std::string bodyOf(const std::string& text) {
    const size_t blank = text.find("\n\n");
    if (blank == std::string::npos) return text;
    return text.substr(blank + 2);
}

std::string timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

long long nowEpoch() {
    return static_cast<long long>(std::time(nullptr));
}

std::string makeDocument(const std::string& kind, const std::string& title,
                         const std::string& body) {
    std::ostringstream out;
    out << "# story: " << kind << "\n";
    if (!title.empty()) out << "# title: " << title << "\n";
    out << "# updated: " << timestamp() << "\n\n";
    out << body;
    if (!body.empty() && body.back() != '\n') out << "\n";
    return out.str();
}

bool isStoryFile(const std::string& name) {
    const std::string suffix = ".mem";
    if (name.size() <= suffix.size()) return false;
    if (name.size() >= 5 &&
        name.compare(name.size() - 5, 5, ".prev") == 0)
        return false;
    return name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
           0;
}

std::vector<std::pair<long long, std::string>> objectFiles() {
    std::vector<std::pair<long long, std::string>> out;
    DIR* dir = ::opendir(story::rootPath().c_str());
    if (!dir) return out;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (!isStoryFile(name)) continue;
        const std::string path = story::rootPath() + "/" + name;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) continue;
        std::string id = name.substr(0, name.size() - 4);
        out.push_back({static_cast<long long>(st.st_mtime), id});
    }
    ::closedir(dir);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });
    return out;
}

std::string indexPath() { return configSubpath("story-index.mem"); }

std::string archiveDir() { return story::rootPath() + "/archive"; }

std::string archivePath(const std::string& id) {
    return archiveDir() + "/" + id + ".mem";
}

std::vector<IndexEntry> loadIndex() {
    std::vector<IndexEntry> out;
    const std::string text = readFile(indexPath());
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::istringstream row(line);
        std::string field;
        while (std::getline(row, field, '\t')) fields.push_back(field);
        if (fields.size() < 5) continue;
        IndexEntry entry;
        entry.id = fields[0];
        entry.score = std::atoi(fields[1].c_str());
        entry.last = std::atoll(fields[2].c_str());
        entry.open = fields[3] == "1";
        entry.active = fields[4] == "1";
        if (!entry.id.empty()) out.push_back(entry);
    }
    if (!out.empty()) return out;

    // Migration from the first version: active object files with no index.
    for (const auto& file : objectFiles()) {
        IndexEntry entry;
        entry.id = file.second;
        entry.score = 1;
        entry.last = file.first;
        entry.open = true;
        entry.active = true;
        out.push_back(entry);
    }
    return out;
}

bool saveIndex(const std::vector<IndexEntry>& entries) {
    std::ostringstream out;
    out << "# story index v1\n";
    out << "# id\tscore\tlast\topen\tactive\n";
    for (const IndexEntry& entry : entries) {
        out << entry.id << '\t' << entry.score << '\t' << entry.last << '\t'
            << (entry.open ? 1 : 0) << '\t' << (entry.active ? 1 : 0) << '\n';
    }
    ensureConfigDir();
    return writeFileAtomic(indexPath(), out.str());
}

IndexEntry* findEntry(std::vector<IndexEntry>& entries,
                      const std::string& id) {
    for (IndexEntry& entry : entries)
        if (entry.id == id) return &entry;
    return nullptr;
}

bool hasOpenLoop(const std::string& body) {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        std::string key = line;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        const size_t at = key.find("OPEN:");
        if (at == std::string::npos) continue;
        std::string value = line.substr(at + 5);
        value = text_utf8::sanitize(value);
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower.empty() || lower == "none" || lower == "no" ||
            lower == "closed" || lower == "resolved" || lower == "nothing")
            return false;
        return true;
    }
    return false;
}

std::string firstLine(const std::string& body) {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        line = text_utf8::sanitize(line);
        while (!line.empty() &&
               std::isspace(static_cast<unsigned char>(line.front())))
            line.erase(line.begin());
        while (!line.empty() &&
               std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (!line.empty()) return text_utf8::head(line, 160);
    }
    return "(dormant)";
}

} // namespace

namespace story {

std::string rootPath() { return configSubpath("story"); }

std::string lifePath() { return configSubpath("story-memory.mem"); }

std::string objectId(const std::string& object) {
    std::string id = slugify(object);
    if (id.empty() || id == "." || id == "..") return "";
    return id;
}

std::string objectPath(const std::string& object) {
    const std::string id = objectId(object);
    if (id.empty()) return "";
    return rootPath() + "/" + id + ".mem";
}

std::string loadLife() { return bodyOf(readFile(lifePath())); }

std::string loadObject(const std::string& object) {
    const std::string path = objectPath(object);
    return path.empty() ? std::string() : bodyOf(readFile(path));
}

bool writeLife(const std::string& body) {
    if (body.empty()) return false;
    ensureConfigDir();
    const std::string path = lifePath();
    const std::string old = readFile(path);
    if (!old.empty()) writeFileAtomic(path + ".prev", old);
    return writeFileAtomic(path, makeDocument("life", "", body));
}

bool writeObject(const std::string& object, const std::string& body) {
    if (body.empty()) return false;
    const std::string id = objectId(object);
    if (id.empty()) return false;
    ensureConfigDir();
    if (!ensureDir(rootPath())) return false;
    if (!ensureDir(archiveDir())) return false;

    const std::string path = rootPath() + "/" + id + ".mem";
    const std::string archived = archivePath(id);

    std::vector<IndexEntry> index = loadIndex();
    IndexEntry* entry = findEntry(index, id);
    const bool fresh = entry == nullptr;
    if (fresh) {
        IndexEntry created;
        created.id = id;
        index.push_back(created);
        entry = &index.back();
    }

    const std::string old = readFile(path);
    if (!old.empty()) writeFileAtomic(path + ".prev", old);
    // Reopening a dormant branch keeps its old text as .prev rather than
    // silently discarding it.
    const std::string oldArchived = readFile(archived);
    if (!oldArchived.empty()) {
        writeFileAtomic(path + ".prev", oldArchived);
        ::unlink(archived.c_str());
    }
    if (!writeFileAtomic(path, makeDocument(id, object, body))) return false;

    entry->score = fresh ? 1 : std::min(100, entry->score + 1);
    entry->last = nowEpoch();
    entry->open = hasOpenLoop(body);
    entry->active = true;
    saveIndex(index);
    return true;
}

std::vector<std::string> listObjects() {
    std::vector<IndexEntry> index = loadIndex();
    std::sort(index.begin(), index.end(),
              [](const IndexEntry& a, const IndexEntry& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.last > b.last;
              });
    std::vector<std::string> out;
    for (const IndexEntry& entry : index)
        if (entry.active) out.push_back(entry.id);
    return out;
}

void sweep() {
    std::vector<IndexEntry> index = loadIndex();
    if (index.empty()) return;
    const long long now = nowEpoch();
    bool changed = false;
    for (IndexEntry& entry : index) {
        if (!entry.active || entry.open) continue;
        const long long age = now - entry.last;
        const bool quiet = age > 30 * kDay || (entry.score < 3 && age > 14 * kDay);
        if (!quiet) continue;
        const std::string active = rootPath() + "/" + entry.id + ".mem";
        if (::access(active.c_str(), F_OK) != 0) continue;
        ensureDir(archiveDir());
        if (::rename(active.c_str(), archivePath(entry.id).c_str()) == 0) {
            entry.active = false;
            changed = true;
        }
    }
    if (changed) saveIndex(index);
}

std::string renderForPrompt(std::size_t maxChars) {
    const std::string life = text_utf8::tail(loadLife(), maxChars / 2);
    std::ostringstream out;
    if (!life.empty()) out << "# Life story\n\n" << life;
    if (out.tellp() > 0 && life.back() != '\n') out << "\n";

    std::vector<IndexEntry> index = loadIndex();
    std::vector<IndexEntry> active;
    std::vector<IndexEntry> dormant;
    for (const IndexEntry& entry : index)
        (entry.active ? active : dormant).push_back(entry);
    std::sort(active.begin(), active.end(),
              [](const IndexEntry& a, const IndexEntry& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.last > b.last;
              });
    std::sort(dormant.begin(), dormant.end(),
              [](const IndexEntry& a, const IndexEntry& b) {
                  return a.last > b.last;
              });

    std::size_t used = static_cast<std::size_t>(out.tellp());
    std::vector<std::string> omitted;
    bool anyActive = false;
    constexpr std::size_t kMaxActiveBranches = 5;
    std::size_t loadedBranches = 0;
    for (const IndexEntry& entry : active) {
        if (loadedBranches >= kMaxActiveBranches) {
            omitted.push_back(entry.id);
            continue;
        }
        const std::string body = loadObject(entry.id);
        if (body.empty()) continue;
        const std::string block = "\n## Object story: " + entry.id +
                                  " (score " + std::to_string(entry.score) +
                                  (entry.open ? ", open)" : ", closed)") +
                                  "\n\n" + body + "\n";
        if (used + block.size() > maxChars) {
            omitted.push_back(entry.id);
            continue;
        }
        if (!anyActive) {
            out << "\n# Active object stories\n";
            anyActive = true;
        }
        out << block;
        used += block.size();
        ++loadedBranches;
    }
    if (!omitted.empty()) {
        out << "\n# Other active object stories (ids only)\n";
        for (const std::string& id : omitted) out << "- " << id << "\n";
    }

    if (!dormant.empty()) {
        out << "\n# Dormant object stories (reopen with object=<id>)\n";
        for (const IndexEntry& entry : dormant) {
            const std::string body = bodyOf(readFile(archivePath(entry.id)));
            out << "- " << entry.id << " — " << firstLine(body) << "\n";
        }
    }
    return out.str();
}

} // namespace story
