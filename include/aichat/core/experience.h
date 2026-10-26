#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Durable, per-target experience. Three layers, three jobs:
//
//   <config>/self-cognition.sca
//       General understanding: how the world works, rules that hold across
//       tasks, abstract judgement. Not a record store.
//
//   <config>/ai-experiences/<target-id>/<target-id>.desc
//       The detailed account of one experience: target, clarified problem,
//       constraints, context, and expected product. Written when the target
//       starts and not rewritten.
//
//   <config>/ai-experiences/<target-id>/<target-id>.exp
//       Distilled practice learned in this target: purpose -> practice ->
//       evidence. Append-only. This is the part a future session should
//       reuse or extend.
//
//   <config>/ai-experiences/<target-id>/<target-id>.steps
//       The complete operational history: every cycle (including failed
//       actions, rejected previews, guard refusals and delivery failures),
//       close-outs, evicted facts, final outcome and reflection. It keeps
//       the audit trail without turning practice into a diary.
//
//   <config>/ai-experiences/<target-id>/<target-id>.state
//       The current state of the target: frontier, coverage, open gaps, and
//       next move. It is rewritten atomically when the state changes, with
//       the previous version kept as .state.prev. The planner loads it for
//       continuity. The event history belongs in .steps.
//
//   <config>/ai-experiences/<target-id>/<target-id>.journal
//       The first-person record for this target: what was noticed, thought,
//       liked or disliked, and wanted next. Append-only; the newest part is
//       loaded into the planner so the worker does not lose its own voice.
//
// A target can name a stable thread in its .desc (`thread: <thread-id>`).
// Then .state and .journal live under
// <config>/ai-experiences/.threads/<thread-id>/, so they survive target
// changes within the same project. A target without a thread is its own
// thread and keeps the files in its own directory.
//
// A target gets its own directory, so experience from one problem is never
// mixed into another one. `load()` returns the `.desc`, `.exp`, `.state` and
// `.journal` for the target plus a short cross-target practice digest; it
// never dumps `.steps`.
namespace experience {

// Root that contains all experience directories.
std::string rootPath();

// Stable, filesystem-safe identifier for a target. Readable characters are
// kept (including UTF-8); path separators and punctuation become '-'.
std::string targetId(const std::string& target);

// Ensures <root>/<id>/<id>.desc exists for this target. A non-empty `thread`
// records `thread: <slug>` in the .desc, so .state and .journal are shared
// with that thread. Returns the id, or an empty string when the directory
// cannot be created.
std::string begin(const std::string& target, const std::string& description,
                  const std::string& thread = "");

// Appends one entry to <id>.exp. Kept for compatibility; prefer recordLesson
// for practice and recordStep for the ledger. Returns the file path, or an
// empty string on failure.
std::string record(const std::string& target, const std::string& title,
                   const std::string& body);

// Appends one practice lesson (purpose -> practice -> evidence) to <id>.exp
// and a one-line digest to <root>/practice.exp so other targets can see that
// the lesson exists. Returns the .exp path, or an empty string on failure.
std::string recordLesson(const std::string& target, const std::string& body);

// Appends one operational entry to <id>.steps. Returns the .steps path, or an
// empty string on failure.
std::string recordStep(const std::string& target, const std::string& title,
                       const std::string& body);

// Replaces <id>.state with the current state of the target, keeping the
// previous version as <id>.state.prev. Returns the .state path, or an empty
// string on failure. The body should be a complete state, not a delta.
std::string writeState(const std::string& target, const std::string& body);

// Returns just the current .state body, or an empty string when none exists.
std::string loadState(const std::string& target);

// Appends one first-person journal entry to <id>.journal. Returns the .journal
// path, or an empty string on failure.
std::string recordJournal(const std::string& target, const std::string& body);

// Returns the detailed description, the latest practice, the current state,
// and the tail of the first-person journal for this target, plus a bounded
// cross-target practice digest, ready to embed in a prompt. Empty when
// nothing is recorded.
std::string load(const std::string& target);

// Paths used above. stepsPath() is empty when the target id is empty.
std::string stepsPath(const std::string& target);
std::string statePath(const std::string& target);
std::string journalPath(const std::string& target);
std::string practicePath();

// A record that could not be written yet. The queue is persistent, so a
// failed experience write survives a restart.
struct PendingRecord {
    std::string target;
    std::string title;
    std::string body;
    std::string channel; // "step" (default), "practice", or "journal"
};

// ~/.config/ai-chat/pending-experience.mem
std::string pendingPath();

std::vector<PendingRecord> loadPending();

// Tries to record the body in the target's step ledger; on failure appends it
// to the pending queue. Then retries any older pending records. Returns the
// number still pending. `quiet` suppresses diagnostic output; background
// callers use it because the chat prompt owns the terminal.
size_t queue(const std::string& target, const std::string& title,
             const std::string& body, bool quiet = false);

// Same, but for a practice lesson (the .exp channel).
size_t queueLesson(const std::string& target, const std::string& title,
                   const std::string& body, bool quiet = false);

// Same, but for the first-person journal. A state rewrite is not queued:
// if it fails, the previous state is kept and the next observer can retry.
size_t queueJournal(const std::string& target, const std::string& title,
                    const std::string& body, bool quiet = false);

// Retries every pending record. Returns the number still pending.
size_t flushPending();

size_t pendingCount();

// One-time migration from the pre-split layout. Older `.exp` files mixed
// practice lessons with step diaries. This walks every target, moves blocks
// that are not in the purpose/practice/evidence form to `.steps`, keeps real
// practice in `.exp`, and rebuilds the cross-target index. Idempotent:
// running it again finds nothing to move. With `dryRun`, nothing is written
// and `report` lists what would change.
bool migrate(std::string& report, bool dryRun = false);

} // namespace experience
