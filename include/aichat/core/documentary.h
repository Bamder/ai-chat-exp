#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "aichat/core/chat.h"
#include "aichat/core/config.h"

// Documentary memory: a layered, factual record of what happened.
//
// The hierarchy has fixed capacities, from newest to oldest:
//
//   live raw item * 1
//   raw item      * 7   (the live item is the eighth)
//   layer 0       * 4
//   layer 1       * 2
//   last item     * 1   (the Boundary of Forgetting)
//
// The live item is the newest and most detailed record. It accumulates new
// segments while they fit; when it fills, or when a new segment would exceed
// the limit, it is sealed. Eight sealed raw items merge pairwise into four
// layer-0 items; four layer-0 items merge pairwise into two layer-1 items;
// two layer-1 items merge into one last item. Two last items merge into one,
// so the Boundary of Forgetting is rewritten rather than duplicated; the model
// may select important or impressive items from the old boundary and carry
// them into the new one.
//
// Every item has the same size limit, regardless of its layer. Merging first
// removes expendable details, then middle event segments, and finally prefers
// the end over the start when the record still does not fit. Every merge also
// receives the current self-cognition text as guidance for judging what
// matters; that text is never merged into an item.
// Items contain only events, statements, decisions, outcomes, names, dates,
// numbers, paths and open questions; they never contain opinions or
// interpretations.
namespace documentary {

// One size limit for every layer. The word budget is what the merge prompt
// asks for; the byte limit is the hard cap applied to model output.
inline constexpr int ITEM_MAX_WORDS = 500;
inline constexpr std::size_t ITEM_MAX_CHARS = 3000;

struct Item {
    std::string start; // local "YYYY-MM-DD HH:MM"
    std::string end;
    std::string text;
};

struct Memory {
    // The newest item. It is not merged while it is live.
    bool hasLive = false;
    Item live;

    // Older items, each vector oldest first. Capacities are enforced by
    // organize(); a failed merge may leave a vector temporarily above capacity
    // so the next call can retry it.
    std::vector<Item> raw;
    std::vector<Item> layer0;
    std::vector<Item> layer1;
    std::vector<Item> last;
};

// ~/.config/ai-chat/documentary-memory.mem
std::string path();

// Current local time in the item timestamp format.
std::string now();

// Reads the memory file. A missing file yields an empty memory.
Memory load();

// Writes the memory atomically. Returns false when the file cannot be written.
bool save(const Memory& memory);

// Appends `rawItem` to the live item when both fit under ITEM_MAX_CHARS;
// otherwise the old live item is sealed into `raw` and the new segment starts
// a new live item. A live item that reaches the limit is sealed too.
// It does not call the model; call organize() afterwards.
void addRaw(Memory& memory, const Item& rawItem);

// True when a level holds enough items for its next merge batch.
bool needsOrganize(const Memory& memory);

// Merges full batches (8 raw -> 4 layer 0, 4 layer 0 -> 2 layer 1,
// 2 layer 1 -> 1 last, 2 last -> 1 last) until every level is below capacity.
// `selfCognition` is the current .sca text. Every merge uses it only as
// guidance for judging which events and details matter; it is never merged
// into an item. A batch is committed only when all of its model calls succeed,
// so a failure never leaves a half-merged batch. `note` receives a short
// result. Returns false only when a model call or a save failed.
bool organize(const Config& cfg, Memory& memory,
              const std::string& selfCognition, std::string& note);

// Asks the model for the raw documentary record of the messages that are
// about to leave the working context. Returns false when the request fails or
// returns no text.
bool makeRaw(const Config& cfg, const std::vector<Message>& messages,
             const std::string& start, const std::string& end,
             std::string& text);

// Renders the memory, newest first and ending with the Boundary of
// Forgetting. With maxChars > 0, keeps the newest items that fit and always
// keeps the Boundary; use 0 for the full /memory display. Empty when there is
// no memory.
std::string render(const Memory& memory, size_t maxChars = 0);

} // namespace documentary
