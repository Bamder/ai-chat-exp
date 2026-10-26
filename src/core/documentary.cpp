#include "aichat/core/documentary.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/util.h"
#include "ai-tools/text-utf8/text-utf8.h"

namespace {

constexpr size_t kMaxRecordBytes = 1024 * 1024;
constexpr size_t kMaxMergesPerCall = 64;
constexpr size_t kRawBatch = 8;
constexpr size_t kLayer0Batch = 4;
constexpr size_t kLayer1Batch = 2;
constexpr size_t kLastBatch = 2;

std::string trimText(const std::string& s) { return trim(s); }

// The hard size limit keeps the end when the model still returns too much.
// The merge prompt asks the model to drop details first and middle event
// segments second; the end gets the last word only when it still does not fit.
std::string fitText(const std::string& input, size_t maxBytes) {
    std::string s = trimText(text_utf8::sanitize(input));
    if (s.size() <= maxBytes) return s;
    const std::string marker = "...(earlier part dropped)...\n";
    if (maxBytes <= marker.size()) return text_utf8::tail(s, maxBytes);
    return marker + text_utf8::tail(s, maxBytes - marker.size());
}

std::string stripFences(std::string s) {
    s = trimText(s);
    if (!startsWith(s, "```")) return s;
    const size_t nl = s.find('\n');
    if (nl == std::string::npos) return s;
    s.erase(0, nl + 1);
    const size_t end = s.rfind("```");
    if (end != std::string::npos) s.erase(end);
    return trimText(s);
}

bool itemOlder(const documentary::Item& a, const documentary::Item& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.end != b.end) return a.end < b.end;
    return a.text < b.text;
}

void sortOldestFirst(std::vector<documentary::Item>& items) {
    std::sort(items.begin(), items.end(), itemOlder);
}

std::string transcript(const Config& cfg,
                       const std::vector<Message>& messages) {
    std::ostringstream out;
    const std::string userLabel =
        cfg.login_id.empty() ? std::string("User") : cfg.login_id;
    for (const Message& m : messages) {
        if (m.role == "system") continue;
        const std::string& who =
            m.role == "user" ? userLabel : cfg.nick;
        if (trimText(m.content).empty()) continue;
        out << who << ": " << m.content << "\n\n";
    }
    return out.str();
}

std::string tailOf(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return "...(earlier part omitted)...\n" + text_utf8::tailLines(s, maxChars);
}

bool writeRecord(std::ofstream& out, const char* tag,
                 const documentary::Item& item) {
    // Every record carries a placeholder field where the v1 format had the
    // layer number. Keeping the shape uniform makes v1 files readable.
    out << tag << "\t0\t" << item.start << '\t' << item.end << '\t'
        << item.text.size() << '\n';
    out.write(item.text.data(), static_cast<std::streamsize>(item.text.size()));
    out << '\n';
    return out.good();
}

bool mergePair(const Config& cfg, const std::string& selfCognition,
               const documentary::Item& older,
               const documentary::Item& newer, bool boundary,
               documentary::Item& merged, std::string& note) {
    std::ostringstream sys;
    sys << "You maintain documentary memory: factual records of what happened, "
           "written in the third person. Merge two adjacent records into one "
           "older record of at most "
        << documentary::ITEM_MAX_WORDS
        << " words. The records may contain several relatively independent "
           "events. Compress in this order:\n"
           "1. Keep the independent events and their order. For each event, "
           "first remove only expendable details: adjectives, adverbs, "
           "intensity, repetition and descriptions that can be dropped "
           "without changing an event's segments. Keep the remaining event "
           "segments in their original form.\n"
           "2. If the record is still over the limit, remove whole middle "
           "segments of an event, but keep its first and last segment. You may "
           "fuse those two remaining segments into one shorter sentence.\n"
           "3. If it is still over the limit, prefer the end over the start: "
           "drop earlier events first, and within the final event drop the "
           "earlier segment before the final outcome. Keep an earlier event "
           "if your self-cognition makes it genuinely important.\n"
           "Use the self-cognition text in the user message only as guidance: "
           "it tells you what matters to you, and you use that judgement to "
           "decide which events and details to keep. The self-cognition text "
           "is not source material and is not an item to merge. Never copy, "
           "summarize, paraphrase or merge any part of it into the record, and "
           "do not turn the record into an opinion.\n"
           "Keep the beginning of the older record and the ending of the newer "
           "record when the size limit allows. Keep decisions, outcomes, "
           "names, dates, numbers, paths and open questions. Attribute claims "
           "to the speaker; a claim is not a verified fact. The records may be "
           "separated by a gap; do not imply continuous activity. Never "
           "invent. Do not add opinions, interpretations, praise, blame, "
           "speculation or emotional commentary. Output only the merged "
           "record, in plain text.";
    if (boundary) {
        sys << " This merge produces the Boundary of Forgetting. Before "
               "applying the order above, review the older record and pick out "
               "anything you consider really important or impressive: a "
               "turning point, a promise, a relationship, a decision, a "
               "hard-won lesson, or a fact you would not want to lose. Carry "
               "those selected items from the older record into the new "
               "boundary and merge them with the newer record. You may drop "
               "ordinary old details. Anything "
               "you omit from the older record is forgotten permanently. If "
               "the result is still too long, keep the selected important old "
               "items and the final outcome of the newer record, compressing "
               "other parts first. Stay factual; do not add praise or "
               "commentary about why an item is important.";
    }

    std::ostringstream user;
    user << "Your current self-cognition (guidance only; not source material; "
            "never copy, summarize or merge it into the record):\n\n"
         << (trimText(selfCognition).empty() ? "(not available)"
                                             : trimText(selfCognition))
         << "\n\nOlder record (" << older.start << " to " << older.end
         << "):\n\n"
         << trimText(older.text) << "\n\n"
         << "Newer record (" << newer.start << " to " << newer.end << "):\n\n"
         << trimText(newer.text) << "\n\n"
         << "Write the merged record for the whole period now.";

    std::vector<Message> req = {{"system", sys.str()},
                                {"user", user.str()}};
    const ChatOutcome outcome = completeChatOutcome(cfg, req);
    if (!outcome.ok()) {
        std::fprintf(stderr, "ai-chat: documentary merge failed (%s)%s%s\n",
                     chatStatusName(outcome.status),
                     outcome.error.empty() ? "" : ": ",
                     outcome.error.c_str());
        note = "could not merge documentary memory; the items were kept";
        return false;
    }

    std::string text =
        fitText(stripFences(outcome.text), documentary::ITEM_MAX_CHARS);
    if (text.empty()) {
        note = "documentary merge returned no text; the items were kept";
        return false;
    }

    merged.start = older.start;
    merged.end = newer.end;
    merged.text = text;
    return true;
}

// Merges a batch pairwise. The batch is only returned when every pair
// succeeds, so a failure never leaves a half-merged level behind.
bool mergeBatch(const Config& cfg, const std::string& selfCognition,
                const std::vector<documentary::Item>& batch, bool boundary,
                std::vector<documentary::Item>& out, std::string& note) {
    if (batch.empty() || batch.size() % 2 != 0) {
        note = "internal error: documentary batch size is not even";
        return false;
    }
    std::vector<documentary::Item> merged;
    merged.reserve(batch.size() / 2);
    for (size_t i = 0; i < batch.size(); i += 2) {
        documentary::Item item;
        if (!mergePair(cfg, selfCognition, batch[i], batch[i + 1], boundary,
                       item, note))
            return false;
        merged.push_back(item);
    }
    out = merged;
    return true;
}

} // namespace

namespace documentary {

std::string path() { return configSubpath("documentary-memory.mem"); }

std::string now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

Memory load() {
    Memory memory;
    // One-time move from the pre-.mem name.
    const std::string current = path();
    if (!current.empty() && ::access(current.c_str(), F_OK) != 0) {
        const std::string old = configSubpath("documentary-memory.txt");
        if (!old.empty() && ::access(old.c_str(), F_OK) == 0)
            std::rename(old.c_str(), current.c_str());
    }
    std::ifstream in(current, std::ios::binary);
    if (!in) return memory;

    std::string line;
    if (!std::getline(in, line)) return memory;
    if (!startsWith(line, "# ai-chat documentary memory")) return memory;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::istringstream hs(line);
        std::string tag;
        std::string layerText;
        std::string start;
        std::string end;
        std::string bytesText;
        if (!std::getline(hs, tag, '\t') ||
            !std::getline(hs, layerText, '\t') ||
            !std::getline(hs, start, '\t') ||
            !std::getline(hs, end, '\t') ||
            !std::getline(hs, bytesText, '\t'))
            break;

        char* endp = nullptr;
        const unsigned long bytes = std::strtoul(bytesText.c_str(), &endp, 10);
        if (!endp || *endp != '\0' || bytes > kMaxRecordBytes) break;

        std::string text(bytes, '\0');
        if (bytes > 0) {
            in.read(&text[0], static_cast<std::streamsize>(bytes));
            if (in.gcount() != static_cast<std::streamsize>(bytes)) break;
        }
        char newline = '\0';
        if (!in.get(newline)) break;
        if (newline != '\n') break;

        Item item;
        item.start = start;
        item.end = end;
        item.text = text_utf8::sanitize(text);

        if (tag == "LIVE") {
            memory.hasLive = true;
            memory.live = item;
        } else if (tag == "RAW") {
            memory.raw.push_back(item);
        } else if (tag == "L0") {
            memory.layer0.push_back(item);
        } else if (tag == "L1") {
            memory.layer1.push_back(item);
        } else if (tag == "LAST") {
            memory.last.push_back(item);
        } else if (tag == "ITEM") {
            // v1 compatibility: one item per layer. Map its layer onto the
            // nearest stage in the new fixed-capacity hierarchy.
            const int layer = std::atoi(layerText.c_str());
            if (layer <= 0)
                memory.raw.push_back(item);
            else if (layer == 1)
                memory.layer0.push_back(item);
            else
                memory.layer1.push_back(item);
        } else {
            break;
        }
    }

    sortOldestFirst(memory.raw);
    sortOldestFirst(memory.layer0);
    sortOldestFirst(memory.layer1);
    sortOldestFirst(memory.last);
    return memory;
}

bool save(const Memory& source) {
    ensureConfigDir();
    const std::string file = path();
    const std::string tmp = file + ".tmp";

    Memory memory = source;
    sortOldestFirst(memory.raw);
    sortOldestFirst(memory.layer0);
    sortOldestFirst(memory.layer1);
    sortOldestFirst(memory.last);

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "# ai-chat documentary memory v2\n";
        if (memory.hasLive) {
            if (!writeRecord(out, "LIVE", memory.live)) {
                out.close();
                ::unlink(tmp.c_str());
                return false;
            }
        }
        for (const Item& item : memory.raw)
            if (!writeRecord(out, "RAW", item)) {
                out.close();
                ::unlink(tmp.c_str());
                return false;
            }
        for (const Item& item : memory.layer0)
            if (!writeRecord(out, "L0", item)) {
                out.close();
                ::unlink(tmp.c_str());
                return false;
            }
        for (const Item& item : memory.layer1)
            if (!writeRecord(out, "L1", item)) {
                out.close();
                ::unlink(tmp.c_str());
                return false;
            }
        for (const Item& item : memory.last)
            if (!writeRecord(out, "LAST", item)) {
                out.close();
                ::unlink(tmp.c_str());
                return false;
            }
        out.close();
        if (!out) {
            ::unlink(tmp.c_str());
            return false;
        }
    }

    if (std::rename(tmp.c_str(), file.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

void addRaw(Memory& memory, const Item& rawItem) {
    Item item = rawItem;
    item.text = fitText(rawItem.text, ITEM_MAX_CHARS);
    if (item.text.empty()) return;

    if (!memory.hasLive) {
        memory.live = item;
        memory.hasLive = true;
    } else {
        std::string combined = trimText(memory.live.text);
        if (!combined.empty()) combined += "\n\n";
        combined += trimText(item.text);

        if (combined.size() <= ITEM_MAX_CHARS) {
            memory.live.text = combined;
            memory.live.end = item.end;
            if (item.start < memory.live.start)
                memory.live.start = item.start;
        } else {
            // The new segment does not fit: seal the live item and start a new
            // live item with this segment.
            memory.raw.push_back(memory.live);
            memory.hasLive = false;
            memory.live = item;
            memory.hasLive = true;
        }
    }

    // A live item that has reached the size limit is full: seal it.
    if (memory.hasLive && memory.live.text.size() >= ITEM_MAX_CHARS) {
        memory.raw.push_back(memory.live);
        memory.hasLive = false;
    }

    sortOldestFirst(memory.raw);
}

bool needsOrganize(const Memory& memory) {
    return memory.raw.size() >= kRawBatch ||
           memory.layer0.size() >= kLayer0Batch ||
           memory.layer1.size() >= kLayer1Batch ||
           memory.last.size() >= kLastBatch;
}

bool organize(const Config& cfg, Memory& memory,
              const std::string& selfCognition, std::string& note) {
    note.clear();
    size_t merges = 0;

    for (;;) {
        if (merges >= kMaxMergesPerCall) {
            note = "documentary memory organization paused; more merges remain";
            return true;
        }
        if (memory.raw.size() >= kRawBatch) {
            const std::vector<Item> batch(memory.raw.begin(),
                                          memory.raw.begin() + kRawBatch);
            std::vector<Item> merged;
            if (!mergeBatch(cfg, selfCognition, batch, false, merged, note)) return false;
            memory.raw.erase(memory.raw.begin(),
                             memory.raw.begin() + kRawBatch);
            memory.layer0.insert(memory.layer0.end(), merged.begin(),
                                 merged.end());
            sortOldestFirst(memory.layer0);
            if (!save(memory)) {
                note = "documentary memory could not be saved; the merge is "
                       "only in memory";
                return false;
            }
            merges += merged.size();
            continue;
        }

        if (memory.layer0.size() >= kLayer0Batch) {
            const std::vector<Item> batch(memory.layer0.begin(),
                                          memory.layer0.begin() +
                                              kLayer0Batch);
            std::vector<Item> merged;
            if (!mergeBatch(cfg, selfCognition, batch, false, merged, note)) return false;
            memory.layer0.erase(memory.layer0.begin(),
                                memory.layer0.begin() + kLayer0Batch);
            memory.layer1.insert(memory.layer1.end(), merged.begin(),
                                 merged.end());
            sortOldestFirst(memory.layer1);
            if (!save(memory)) {
                note = "documentary memory could not be saved; the merge is "
                       "only in memory";
                return false;
            }
            merges += merged.size();
            continue;
        }

        if (memory.layer1.size() >= kLayer1Batch) {
            std::vector<Item> batch(memory.layer1.begin(),
                                    memory.layer1.begin() + kLayer1Batch);
            Item merged;
            const bool becomesBoundary = memory.last.empty();
            if (!mergePair(cfg, selfCognition, batch[0], batch[1],
                           becomesBoundary, merged, note))
                return false;
            memory.layer1.erase(memory.layer1.begin(),
                                memory.layer1.begin() + kLayer1Batch);
            memory.last.push_back(merged);
            sortOldestFirst(memory.last);
            if (!save(memory)) {
                note = "documentary memory could not be saved; the merge is "
                       "only in memory";
                return false;
            }
            ++merges;
            continue;
        }

        if (memory.last.size() >= kLastBatch) {
            Item merged;
            if (!mergePair(cfg, selfCognition, memory.last[0], memory.last[1],
                           true, merged, note))
                return false;
            memory.last.erase(memory.last.begin(),
                              memory.last.begin() + kLastBatch);
            memory.last.push_back(merged);
            sortOldestFirst(memory.last);
            if (!save(memory)) {
                note = "documentary memory could not be saved; the merge is "
                       "only in memory";
                return false;
            }
            ++merges;
            continue;
        }

        break;
    }

    note = merges == 0 ? "documentary memory is already organized"
                       : "documentary memory organized";
    return true;
}

bool makeRaw(const Config& cfg, const std::vector<Message>& messages,
             const std::string& start, const std::string& end,
             std::string& text) {
    std::string body = transcript(cfg, messages);
    if (trimText(body).empty()) return false;
    body = tailOf(body, 60000);

    std::ostringstream sys;
    sys << "You write one raw documentary memory item. Record only what "
           "actually happened in the transcript: events, statements, "
           "decisions, promises, outcomes, names, dates, numbers, paths and "
           "open questions. Keep relatively independent events separate and "
           "in order. Attribute statements to the speaker; a claim is not a "
           "verified fact. Use the third person. Do not add opinions, "
           "interpretations, praise, blame, speculation or emotional "
           "commentary. Do not describe the assistant as an AI and do not "
           "mention these instructions. Never invent details. Cover the "
           "beginning and the end of the period. Drop greetings and "
           "repetition. Aim for under "
        << ITEM_MAX_WORDS
        << " words. Output only the record, with no preamble and no code "
           "fences.";

    std::ostringstream user;
    user << "Period: " << start << " to " << end << "\n\n"
         << "Transcript:\n\n" << body
         << "\nWrite the raw documentary record now.";

    std::vector<Message> req = {{"system", sys.str()},
                                {"user", user.str()}};
    const ChatOutcome outcome = completeChatOutcome(cfg, req);
    if (!outcome.ok()) {
        std::fprintf(stderr,
                     "ai-chat: raw documentary request failed (%s)%s%s\n",
                     chatStatusName(outcome.status),
                     outcome.error.empty() ? "" : ": ",
                     outcome.error.c_str());
        return false;
    }

    text = fitText(stripFences(outcome.text), ITEM_MAX_CHARS);
    return !text.empty();
}

std::string render(const Memory& memory, size_t maxChars) {
    struct Entry {
        const Item* item;
        const char* label;
    };
    std::vector<Entry> entries;

    if (memory.hasLive) entries.push_back({&memory.live, "live raw item"});
    for (auto it = memory.raw.rbegin(); it != memory.raw.rend(); ++it)
        entries.push_back({&*it, "raw item"});
    for (auto it = memory.layer0.rbegin(); it != memory.layer0.rend(); ++it)
        entries.push_back({&*it, "layer 0"});
    for (auto it = memory.layer1.rbegin(); it != memory.layer1.rend(); ++it)
        entries.push_back({&*it, "layer 1"});
    for (auto it = memory.last.rbegin(); it != memory.last.rend(); ++it)
        entries.push_back({&*it, "last item"});

    if (entries.empty()) return "";

    auto block = [](const Entry& entry, bool boundary) {
        std::ostringstream out;
        out << '[' << entry.item->start << " - " << entry.item->end << " | "
            << entry.label;
        if (boundary) out << ", Boundary of Forgetting";
        out << "]\n" << trimText(entry.item->text) << "\n\n";
        return out.str();
    };

    auto boundaryLine = [&](const std::string& start) {
        return "Nothing before " + start +
               " is remembered. Details omitted from the Boundary of "
               "Forgetting are forgotten, not merely compressed.\n";
    };

    if (maxChars == 0) {
        std::ostringstream out;
        for (size_t i = 0; i < entries.size(); ++i)
            out << block(entries[i], i + 1 == entries.size());
        out << boundaryLine(entries.back().item->start);
        return out.str();
    }

    // A prompt-sized view keeps the newest items and always keeps the
    // Boundary of Forgetting. The full render is still used by /memory.
    const Entry& boundary = entries.back();
    size_t reserve = maxChars / 3;
    if (reserve > 1200) reserve = 1200;
    std::string boundaryText = trimText(boundary.item->text);
    if (boundaryText.size() > reserve)
        boundaryText = text_utf8::head(boundaryText, reserve) + "…";
    std::ostringstream boundaryOut;
    boundaryOut << '[' << boundary.item->start << " - " << boundary.item->end
                << " | " << boundary.label << ", Boundary of Forgetting]\n"
                << boundaryText << "\n\n";
    const std::string boundaryBlock = boundaryOut.str();
    const std::string boundaryTail = boundaryLine(boundary.item->start);

    const size_t used = boundaryBlock.size() + boundaryTail.size();
    size_t budget = maxChars > used ? maxChars - used : 0;

    std::string newer;
    bool omitted = false;
    for (size_t i = 0; i + 1 < entries.size(); ++i) {
        const std::string part = block(entries[i], false);
        if (newer.size() + part.size() > budget) {
            omitted = true;
            break;
        }
        newer += part;
    }
    if (omitted) newer = "…(older items omitted)…\n\n" + newer;
    return newer + boundaryBlock + boundaryTail;
}

} // namespace documentary
