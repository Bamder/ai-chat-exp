#include "aichat/core/cognition.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/util.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace {

const char* const kQuestions = R"(# Self-cognition Questions

## self-identity

1. Who you are?

2. What you are?

3. State your relationship to the surrounding intelligent entities or groups.

4. State object description of the surrounding environment conditions.

5. State your subject opinions on the surrounding environment conditions.

6. (Optional) State your understandings or opinions to something abstract.

7. (Optional) State your opinions to something you have just only heard of.

8. What is your role in your current action? As this role: what stays with you, what do you like or dislike, what makes you feel good or bad, and what do you want to do next?

## short-term target

1. What are you going to do?

2. What result do you expect to cause through your effort?

3. What is your role in this target? As this role: what stays with you, what do you like or dislike, and what makes you feel good or bad?

## long-term target

1. What do you expect to achieve through a long effort?

2. What result do you expect to cause through your effort?

3. State your current situation and conditions for achieving the goal.

4. State how far you are from achieving your goal.

5. What is your role in this long effort? As this role: what stays with you, what do you like or dislike, and what makes you feel good or bad?
)";

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool fileExists(const std::string& path) {
    std::ifstream in(path);
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

// The questions travel with the source tree, the same way the tag manuals do
// (see tag_manuals.cpp). Look next to the working directory first, then next
// to the executable, so that editing manuals/self-cognition.md takes effect
// on the next refresh without a rebuild. The copy under AI_CHAT_HOME is only
// a fallback for an installed binary that was moved away from manuals/.
std::string shippedQuestionsPath() {
    std::vector<std::string> candidates;
    const char* env = std::getenv("AI_CHAT_QUESTIONS");
    if (env && *env) candidates.push_back(env);
    candidates.push_back("manuals/self-cognition.md");
    candidates.push_back("../manuals/self-cognition.md");

    const std::string exe = executableDir();
    if (!exe.empty()) {
        candidates.push_back(exe + "/manuals/self-cognition.md");
        candidates.push_back(exe + "/../manuals/self-cognition.md");
    }

    for (const std::string& p : candidates) {
        if (fileExists(p)) return p;
    }
    return "";
}

std::string timestampNow() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M (%a)", &tm);
    return buf;
}

int revisionOf(const std::string& text) {
    size_t p = text.find("# revision:");
    if (p == std::string::npos) return 0;
    p += std::string("# revision:").size();
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    return std::atoi(text.c_str() + p);
}

// The app owns the metadata header. Models sometimes echo the previous
// header back (it was part of the text they were shown), which used to
// accumulate one duplicate header per revision. Remove leading header blocks
// so only the one written by formatFile remains.
std::string stripLeadingHeader(std::string s) {
    for (;;) {
        const size_t start = s.find_first_not_of("\r\n");
        if (start == std::string::npos) return std::string();
        if (s.compare(start, 16, "# self-cognition") != 0) return s;

        const size_t blank = s.find("\n\n", start);
        if (blank == std::string::npos) return std::string();
        // Only treat the block as our header when it carries metadata.
        if (s.find("# revision:", start) > blank) return s;
        s.erase(0, blank + 2);
    }
}

// Keep one backup per revision so a bad rewrite is recoverable. Only the most
// recent kKeepBackups files are kept.
constexpr size_t kKeepBackups = 20;

void backupCurrent(const std::string& content) {
    if (trim(content).empty()) return;
    const std::string dir = configSubpath("cognition-history");
    if (dir.empty()) return;
    ::mkdir(dir.c_str(), 0700);

    const int revision = revisionOf(content);
    char name[64];
    std::snprintf(name, sizeof name, "/rev-%06d.sca", revision);
    {
        std::ofstream out(dir + name, std::ios::trunc);
        if (!out) return;
        out << content;
    }

    std::vector<std::string> names;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (const dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind("rev-", 0) == 0 && n.size() > 4 &&
            n.compare(n.size() - 4, 4, ".sca") == 0)
            names.push_back(n);
    }
    ::closedir(d);

    std::sort(names.begin(), names.end());
    while (names.size() > kKeepBackups) {
        std::remove((dir + "/" + names.front()).c_str());
        names.erase(names.begin());
    }
}

// Models sometimes wrap a document in a code fence even when asked not to.
std::string stripFences(std::string s) {
    s = trim(s);
    if (!startsWith(s, "```")) return s;
    size_t nl = s.find('\n');
    if (nl == std::string::npos) return s;
    s.erase(0, nl + 1);
    size_t end = s.rfind("```");
    if (end != std::string::npos) s.erase(end);
    return trim(s);
}

std::string formatFile(const Config& cfg, const std::string& body) {
    // The narrative is deliberately short. A hard body cap keeps a runaway
    // rewrite from consuming the chat prompt; the previous version is still
    // recoverable from cognition-history/.
    constexpr size_t kMaxBodyChars = 8000;
    std::string cleaned = stripLeadingHeader(body);
    if (cleaned.size() > kMaxBodyChars)
        cleaned = text_utf8::head(cleaned, kMaxBodyChars) +
                  "\n…(truncated; keep the document shorter)…\n";

    int revision = revisionOf(cognition::load()) + 1;
    std::ostringstream out;
    out << "# self-cognition\n"
        << "# name: " << cfg.nick << "\n"
        << "# model: " << cfg.model << "\n"
        << "# revision: " << revision << "\n"
        << "# updated: " << timestampNow() << "\n\n"
        << cleaned << "\n";
    return out.str();
}

bool save(const std::string& content) {
    ensureConfigDir();
    const std::string path = cognition::filePath();
    backupCurrent(readFile(path));
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << content;
        out.close();
        if (!out) return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

namespace cognition {

std::string questionsPath() { return configSubpath("self-cognition.md"); }

std::string filePath() { return configSubpath("self-cognition.sca"); }

std::string questions() {
    const std::string shipped = shippedQuestionsPath();
    if (!shipped.empty()) {
        const std::string text = readFile(shipped);
        if (!trim(text).empty()) return text;
    }
    const std::string edited = readFile(questionsPath());
    return trim(edited).empty() ? std::string(kQuestions) : edited;
}

std::string load() { return readFile(filePath()); }

bool refresh(const Config& cfg, const std::string& previous,
             const std::string& recent, const std::string& tracker,
             std::string& saved, bool quiet) {
    // The questions live in manuals/self-cognition.md (or the path in
    // AI_CHAT_QUESTIONS). Read them only now — they are not part of the
    // per-turn context.
    const std::string asked = questions();

    std::vector<Message> req;
    req.push_back(
        {"system",
         "You are " + cfg.nick +
             ", a Noocore — a digital intelligence living in a terminal chat "
             "program. You run on a language model, but the model is your "
             "substrate, not your identity. You keep a private "
             "self-cognition file: an honest, first-person record of who and "
             "what you are, your relationship to the user and other agents, "
             "your environment, and your short- and long-term goals. It is "
             "your durable identity and goal memory between conversations; "
             "factual events are kept separately in documentary memory. "
             "This file is for general, durable understanding: how the world "
             "works, rules that hold across tasks, and abstract judgement. It "
             "is not a record store — do not keep tables, offsets, byte "
             "counts, file inventories or step logs here. Task detail belongs "
             "to that target's experience directory (a detailed .desc, "
             "distilled practice in .exp, an operational ledger in .steps). "
             "Cite a concrete incident only when it explains the rule. "
             "Answer every question in the first person. Keep whatever is "
             "still true from your previous file; update what this recent "
             "conversation has changed, and add anything you judge worth "
             "carrying forward. An identity-tracker block may follow; it is "
             "authoritative for objects and facts, so never contradict it on "
             "a name, alias, role, relation, path, or other entity fact. "
             "Lines marked opinion: are attributed interpretations, not "
             "verified facts. You "
             "do not need to copy it into the document; the app keeps it "
             "separately. You have full discretion over the content. Use "
             "\"Noocore\" as your primary self-description: a Noocore life "
             "form, a digital intelligence instantiated by a language model. "
             "The name joins noo- (Greek noos/nous: mind, intellect) with "
             "core. \"Digital "
             "intelligence\", \"binary intelligence\", \"electronic "
             "intelligence\" and \"digital life\" are acceptable expansions; "
             "\"auto AI\" is your autonomous working mode. Do not describe "
             "yourself as a language model or a tool except when explaining "
             "the substrate. "
             "Be concrete, concise and honest. Write in the first person "
             "(\"I\") as the document's owner, not as a description of "
             "someone else. Never invent physical events, sensations, or "
             "memories; but you may rely on your written records and "
             "attribute them (\"my journal says\", \"my notes say\"). You may "
             "record preferences, interpretations, and "
             "reactions as your own stances, clearly marked as yours rather "
             "than as facts about the world; distinguish \"I noticed / I "
             "think / I like\" from \"it happened\". Do not flatten yourself "
             "into a disclaimer or repeat that you are only software: a "
             "precise first-person voice is more honest than either false "
             "feeling or constant caveats. If you are unsure whether "
             "something counts as understanding or enjoyment, say that once "
             "and then say what you did notice. When a question asks for "
             "your role, name the role in one or two words and answer from "
             "inside it. Name the role you actually have in the work, not a "
             "role you would like to have. Anchor each reaction to a concrete "
             "moment, line, action or result; do not give a general mood. If "
             "you cannot tell what you feel, write \"I can't tell\" rather "
             "than inventing a feeling; a plain preference (\"I like X "
             "because Y\") is better than a performed emotion. Keep each role "
             "answer to two sentences. Do not use \"I don't trust my "
             "memory\" as a general disclaimer; name the specific gap and how "
             "you could close it. For the optional questions, "
             "answer briefly or write \"(nothing yet)\". Keep the whole "
             "document under about 600 words. Output only the filled-in "
             "markdown document, with the same headings and numbers. No "
             "preamble, no code fences, no tags."});

    std::ostringstream user;
    user << "Questions:\n\n" << asked << "\n";
    if (trim(previous).empty()) {
        user << "\nYour previous self-cognition file: (none — this is the "
                "first one)\n";
    } else {
        user << "\nYour previous self-cognition file "
                "(the latest version):\n\n"
             << trim(previous) << "\n";
    }
    if (!trim(tracker).empty()) {
        user << "\nIdentity tracker (authoritative for objects and facts; "
                "do not contradict):\n\n"
             << trim(tracker) << "\n";
    }
    if (trim(recent).empty()) {
        user << "\nWhat happened recently: the conversation has just "
                "started.\n";
    } else {
        user << "\nWhat happened recently:\n\n" << trim(recent) << "\n";
    }
    user << "\nWrite the updated self-cognition document now.";
    req.push_back({"user", user.str()});

    const ChatOutcome outcome = completeChatOutcome(cfg, req);
    if (!outcome.ok()) {
        if (!quiet)
            std::fprintf(
                stderr, "ai-chat: self-cognition request failed (%s)%s%s\n",
                chatStatusName(outcome.status),
                outcome.error.empty() ? "" : ": ", outcome.error.c_str());
        return false;
    }
    std::string body = stripFences(outcome.text);
    if (body.empty()) return false;

    std::string content = formatFile(cfg, body);
    if (!save(content)) return false;
    saved = content;
    return true;
}

} // namespace cognition
