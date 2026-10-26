#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The identity tracker: durable, object-oriented world memory.
//
// Each object has a category, a canonical name, an id, and a list of
// identities (aliases, roles, relations, facts, opinions, notes, and an
// optional login binding). It is append-only in spirit: an object is never
// hard-deleted, only merged into another object or retired. The file is
// identity-tracker.mem.
//
// This is not self-cognition. self-cognition.sca is the AI's own narrative
// and guidance; the tracker is the authoritative record of the objects in its
// world and what is true about them.
namespace identity {

// How many merged/retired objects stay in the active file for undo. Older
// tombstones move to identity-history.mem.
inline constexpr size_t kKeepTombstones = 40;

struct Line {
    std::string kind; // alias, role, relation, fact, opinion, note, login
    std::string text;
};

struct Object {
    std::string id;
    std::string category = "?";
    std::string name;
    std::string status = "active"; // active, merged-into:<id>, retired
    std::string created;
    std::string source = "user";
    std::vector<Line> lines;
    std::vector<std::string> mergedFrom;
    std::string mergeReason;
};

struct Tracker {
    std::vector<Object> objects;
};

// Inputs for one rendered view. `focus` is the app-owned anchor list (for
// example the user object's name in chat, or keywords from the current task in
// autonomous mode). It is never filled by the model.
struct ViewContext {
    std::string user_message;
    std::string recent;
    std::string task;
    std::vector<std::string> focus;
};

struct View {
    std::string text;
    size_t detailed = 0;
    size_t indexed = 0;
    size_t omitted = 0;
};

// ~/.config/ai-chat/identity-tracker.mem
std::string path();

// Reads the tracker. Also performs the one-time migration of old identity
// facts from autonomy-facts.mem, if any exist.
Tracker load();

// Writes the tracker atomically.
bool save(const Tracker& tracker);

// Archived merged/retired objects. Not rendered and not part of the prompt.
std::string historyPath();

// Moves all but the newest `keepRecent` merged/retired objects out of the
// active tracker and into identity-history.mem. Returns false only when the
// archive cannot be written. The caller saves the modified active tracker.
bool compact(Tracker& tracker, size_t keepRecent, std::string& note);

// Renders all active objects, compactly. Used by /track and the .sca refresh.
// Empty when the tracker has no active objects.
std::string render(const Tracker& tracker);

// Renders a relevance-selected view for the chat prompt:
//
//   details: objects matching the focus, the user message, the task or recent
//            turns, plus the AI's own object, until the detail budget is full.
//   index:   one line for every remaining active object, so the model still
//            knows the object exists even when its details are withheld.
//
// Selection is deterministic: score first, then object id. Empty when there
// are no active objects.
View renderView(const Tracker& tracker, const ViewContext& ctx,
                size_t detail_budget = 6000, size_t index_budget = 2000);

const Object* find(const Tracker& tracker, const std::string& idOrName);
Object* find(Tracker& tracker, const std::string& idOrName);

// The first active object with a role/relation equal to `role` after
// canonicalisation (e.g. "user", "myself"). nullptr when there is none.
const Object* primaryWithRole(const Tracker& tracker, const std::string& role);

// The active object bound to a login-id, either by a `login:` line or by an
// exact name/alias match on a Human/Person object. nullptr when the login-id
// is unknown. The login-id is a session label, not an identity kind.
const Object* objectForLogin(const Tracker& tracker,
                             const std::string& loginId);

// Binds a login-id to a tracked person. If an object already carries that
// login, nothing changes. Otherwise an active Human/Person object whose name
// or alias matches is bound; when there is none, a new `[Human] <login-id>`
// object with source "login" is created. `note` is empty when the binding
// already existed, otherwise it describes the change. This is app-owned: it
// is not exposed through the <track> tag.
bool bindLogin(Tracker& tracker, const std::string& loginId,
               std::string& note);

// Scans `text` for names/aliases of active objects and returns up to `max`
// canonical names, most specific match first. This is the app's deterministic
// way to derive a short focus list from a task, log, or incoming message.
std::vector<std::string> focusFromText(const Tracker& tracker,
                                       const std::string& text,
                                       size_t max = 3);

// Parses `[Category] Name: identity; identity; ...` and creates or updates the
// object. Items may carry an explicit alias:, role:, relation:, fact:,
// opinion: or note: prefix; otherwise Human/AI items become roles and the rest
// become facts. Returns the object id in `id` and a human-readable result in
// `note`.
bool track(Tracker& tracker, const std::string& spec, std::string& id,
           std::string& note);

// Appends one identity to an existing active object. `kind` must be one of
// alias, role, relation, fact, opinion or note. Used by the <track .../> tag;
// the app validates the object, trims the text and dedupes.
bool addLine(Tracker& tracker, const std::string& idOrName,
             const std::string& kind, const std::string& text,
             std::string& note);

// Merges `from` into `into`: unions their identities, records the merge on the
// target, and marks the source as merged-into. Nothing is hard-deleted.
bool merge(Tracker& tracker, const std::string& from, const std::string& into,
           const std::string& reason, std::string& note);

// Marks an object retired. It stays in the file for audit and is hidden from
// the rendered prompt.
bool forget(Tracker& tracker, const std::string& idOrName, std::string& note);

} // namespace identity
