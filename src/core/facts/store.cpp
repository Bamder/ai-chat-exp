#include "aichat/core/facts/store.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/facts/fallback.h"
#include "aichat/core/facts/text.h"
#include "aichat/core/facts/verifier.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace facts {

namespace {

constexpr size_t kMaxFacts = 80;
constexpr size_t kMaxSuperseded = 12;
constexpr size_t kRenderChars = 8000;

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

bool validKind(int v) {
    return v >= static_cast<int>(FactKind::Generic) &&
           v <= static_cast<int>(FactKind::Failure);
}

bool validStatus(int v) {
    return v >= static_cast<int>(FactStatus::Verified) &&
           v <= static_cast<int>(FactStatus::Superseded);
}

Fact parseFact(const std::vector<std::string>& p) {
    Fact f;
    if (p.size() < 9) return f;
    const int kind = std::atoi(p[4].c_str());
    const int status = std::atoi(p[5].c_str());
    if (!validKind(kind) || !validStatus(status)) return f;
    f.key = unescapeField(p[1]);
    f.subject = unescapeField(p[2]);
    f.value = unescapeField(p[3]);
    f.kind = static_cast<FactKind>(kind);
    f.status = static_cast<FactStatus>(status);
    f.source = unescapeField(p[6]);
    f.check = unescapeField(p[7]);
    f.cycle = std::atoi(p[8].c_str());
    return f;
}

// Read the whole store without deciding which target it belongs to.
struct FileState {
    std::string target;
    std::string doneWhen;
    int revision = 0;
    int lastCycle = 0;
    std::vector<Fact> facts;
    std::vector<Fact> superseded;
};

FileState readStore(const std::string& storage) {
    FileState state;
    std::ifstream in(storage);
    if (!in) return state;

    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("target\t", 0) == 0) {
            state.target = unescapeField(line.substr(7));
        } else if (line.rfind("done_when\t", 0) == 0) {
            state.doneWhen = unescapeField(line.substr(10));
        } else if (line.rfind("revision\t", 0) == 0) {
            state.revision = std::atoi(line.substr(9).c_str());
        } else if (line.rfind("last_cycle\t", 0) == 0) {
            state.lastCycle = std::atoi(line.substr(11).c_str());
        } else if (line.rfind("fact\t", 0) == 0) {
            Fact f = parseFact(splitTabs(line));
            if (!f.key.empty()) state.facts.push_back(std::move(f));
        } else if (line.rfind("old\t", 0) == 0) {
            Fact f = parseFact(splitTabs(line));
            if (!f.key.empty()) {
                f.status = FactStatus::Superseded;
                state.superseded.push_back(std::move(f));
            }
        }
    }
    return state;
}

// A one-time move from the pre-.mem name. Only the default store is migrated;
// tests and callers with explicit paths are untouched.
void migrateLegacy(const std::string& storage) {
    if (storage.empty() || storage != configSubpath("autonomy-facts.mem")) return;
    if (::access(storage.c_str(), F_OK) == 0) return;
    const std::string old = configSubpath("autonomy-facts.txt");
    if (old.empty() || ::access(old.c_str(), F_OK) != 0) return;
    std::rename(old.c_str(), storage.c_str());
}

bool isIssue(const Fact& f) {
    return f.kind == FactKind::Failure || f.kind == FactKind::Rejection;
}

std::string factLine(const Fact& f) {
    std::ostringstream out;
    out << "- [" << kindName(f.kind) << "] " << f.key << " = " << f.value
        << " (cycle " << f.cycle;
    if (!f.source.empty()) out << "; " << f.source;
    if (!f.check.empty()) out << "; " << f.check;
    out << ")\n";
    return out.str();
}

} // namespace

std::string Store::defaultPath() {
    return configSubpath("autonomy-facts.mem");
}

Store::Store(std::string storagePath) : storage_(std::move(storagePath)) {
    if (storage_.empty()) storage_ = defaultPath();
}

void Store::reset() {
    revision_ = 0;
    lastCycle_ = 0;
    changeVersion_ = 0;
    facts_.clear();
    superseded_.clear();
    evicted_.clear();
}

void Store::load() {
    migrateLegacy(storage_);
    reset();

    FileState state = readStore(storage_);
    target_ = state.target;
    doneWhen_ = state.doneWhen;
    revision_ = state.revision;
    lastCycle_ = state.lastCycle;
    facts_ = std::move(state.facts);
    superseded_ = std::move(state.superseded);
}

void Store::begin(const std::string& target, const std::string& doneWhen) {
    target_ = compact(target, 1000);
    doneWhen_ = compact(doneWhen, 1000);
    reset();
    migrateLegacy(storage_);

    FileState state = readStore(storage_);
    // Resume only for the same target; a new target starts fresh.
    if (!state.target.empty() && compact(state.target, 1000) == target_) {
        facts_ = std::move(state.facts);
        superseded_ = std::move(state.superseded);
        revision_ = state.revision;
        lastCycle_ = state.lastCycle;
        if (doneWhen_.empty()) doneWhen_ = compact(state.doneWhen, 1000);
    }

    save();
}

void Store::apply(int cycle, const std::string& action,
                  const std::map<std::string, std::string>& args,
                  const action::Result& result) {
    ++revision_;
    lastCycle_ = cycle;

    std::vector<Fact> collected = fallback(action, args, result, cycle);
    verify(collected);
    for (Fact& f : collected) merge(std::move(f));

    save();
}

void Store::rejected(int cycle, const std::string& action,
                     const std::map<std::string, std::string>& args,
                     const std::string& reason) {
    ++revision_;
    lastCycle_ = cycle;

    const auto it = args.find("path");
    const std::string path = it == args.end() ? std::string() : trimText(it->second);
    Fact f = make(FactKind::Rejection,
                  "reject:" + action + (path.empty() ? "" : ":" + path), path,
                  compact(reason), FactStatus::Reported, "preview");
    f.source = action;
    f.cycle = cycle;
    merge(std::move(f));

    save();
}

void Store::merge(Fact fact) {
    for (Fact& existing : facts_) {
        if (existing.key != fact.key) continue;

        const bool changed = existing.value != fact.value ||
                             existing.status != fact.status ||
                             existing.kind != fact.kind;
        if (changed) ++changeVersion_;

        // A changed value of a state fact is a real update: keep the old
        // version as superseded, so a change is visible instead of silent.
        // Run/listing results are snapshots, not contradictions; replace them
        // without filling the history with every invocation.
        const bool stateKind =
            fact.kind == FactKind::FsFile || fact.kind == FactKind::FsDir ||
            fact.kind == FactKind::Wrote || fact.kind == FactKind::Mirror;
        if (stateKind && existing.value != fact.value) {
            Fact old = existing;
            old.status = FactStatus::Superseded;
            superseded_.push_back(std::move(old));
            if (superseded_.size() > kMaxSuperseded) {
                // The worker archives this trace in the target's experience.
                evicted_.push_back(superseded_.front());
                superseded_.erase(superseded_.begin());
            }
        }
        existing = std::move(fact);
        return;
    }

    ++changeVersion_;
    facts_.push_back(std::move(fact));
    if (facts_.size() <= kMaxFacts) return;

    // Evict the oldest fact that is not a standing access grant.
    auto victim = facts_.end();
    for (auto it = facts_.begin(); it != facts_.end(); ++it) {
        if (it->kind == FactKind::Grant) continue;
        if (victim == facts_.end() || it->cycle < victim->cycle) victim = it;
    }
    if (victim != facts_.end()) {
        // The worker archives this trace in the target's experience.
        evicted_.push_back(*victim);
        facts_.erase(victim);
    }
}

std::vector<Fact> Store::takeEvicted() {
    std::vector<Fact> out = std::move(evicted_);
    evicted_.clear();
    return out;
}

const Fact* Store::find(const std::string& key) const {
    for (const Fact& f : facts_)
        if (f.key == key) return &f;
    return nullptr;
}

bool Store::hasVerifiedOutput() const {
    for (const Fact& f : facts_) {
        if (f.status != FactStatus::Verified) continue;
        switch (f.kind) {
        case FactKind::Run:
            // Only a clean run is a product; "exit=1" is an input to the
            // next decision, not evidence of completion.
            if (f.value.rfind("exit=0", 0) == 0) return true;
            break;
        case FactKind::Wrote:
        case FactKind::Mirror:
        case FactKind::Tool:
            return true;
        default:
            break;
        }
    }
    return false;
}

std::string Store::render() const {
    std::ostringstream out;
    out << "fact store revision " << revision_;
    if (lastCycle_ > 0) out << ", updated after cycle " << lastCycle_;
    out << "\ntarget: " << (target_.empty() ? "(none)" : target_) << "\n";
    if (!doneWhen_.empty()) out << "done when: " << doneWhen_ << "\n";

    const auto section = [&out](const char* title,
                                const std::vector<const Fact*>& list) {
        if (list.empty()) return;
        out << "\n" << title << "\n";
        for (const Fact* f : list) out << factLine(*f);
    };

    std::vector<const Fact*> verified;
    std::vector<const Fact*> contradicted;
    std::vector<const Fact*> issues;
    std::vector<const Fact*> reported;

    for (const Fact& f : facts_) {
        if (isIssue(f)) {
            issues.push_back(&f);
        } else if (f.status == FactStatus::Verified) {
            verified.push_back(&f);
        } else if (f.status == FactStatus::Contradicted) {
            contradicted.push_back(&f);
        } else {
            reported.push_back(&f);
        }
    }

    if (verified.empty() && contradicted.empty() && issues.empty() &&
        reported.empty() && superseded_.empty())
        out << "\n(no facts yet)\n";

    // Ordered by decision relevance; the render cap drops from the tail.
    section("VERIFIED FACTS (checked by the app)", verified);
    section("CONTRADICTIONS (a check disagrees with the claim)", contradicted);
    section("FAILURES AND REJECTIONS", issues);
    section("REPORTED (tool output, not independently checked)", reported);
    if (!superseded_.empty()) {
        out << "\nSUPERSEDED (replaced by a newer fact)\n";
        for (const Fact& f : superseded_) out << factLine(f);
    }

    std::string text = out.str();
    if (text.size() > kRenderChars)
        text = text_utf8::head(text, kRenderChars) + "\n…(more facts omitted)…\n";
    return text;
}

bool Store::save() const {
    const std::string temp = storage_ + ".tmp";
    std::ofstream out(temp, std::ios::trunc);
    if (!out) return false;

    out << "# ai-chat fact store v1\n"
        << "target\t" << escapeField(target_) << "\n"
        << "done_when\t" << escapeField(doneWhen_) << "\n"
        << "revision\t" << revision_ << "\n"
        << "last_cycle\t" << lastCycle_ << "\n";

    const auto writeFact = [&out](const char* tag, const Fact& f) {
        out << tag << "\t" << escapeField(f.key) << "\t"
            << escapeField(f.subject) << "\t" << escapeField(f.value) << "\t"
            << static_cast<int>(f.kind) << "\t"
            << static_cast<int>(f.status) << "\t" << escapeField(f.source)
            << "\t" << escapeField(f.check) << "\t" << f.cycle << "\n";
    };
    for (const Fact& f : facts_) writeFact("fact", f);
    for (const Fact& f : superseded_) writeFact("old", f);

    out.close();
    if (!out) {
        std::remove(temp.c_str());
        return false;
    }
    if (std::rename(temp.c_str(), storage_.c_str()) != 0) {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

} // namespace facts
