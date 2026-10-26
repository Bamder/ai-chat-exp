#pragma once

#include <string>

#include "aichat/core/action.h"

// The self-made-tool tool lets the autonomous worker create, inspect, edit
// and run tools of its own. Every tool lives in its own subdirectory under
// <config>/self-made-tools/<name>/. Each change is presented to the user for
// approval first; `add` and `edit` then grant read/write access to that
// subdirectory, so the tool's own file ops (read/write/append/edit/grep/run/
// import) and the normal file actions can work inside it. The granted
// directories are recorded in <config>/self-made-tools/.grants and restored
// on the next startup, so a tool approved once does not need approval again
// in later sessions. Repeating `add` or `edit` for a folder that already has
// a grant is a no-op and does not prompt again.
//
// Reads under <config>/self-made-tools/ are always allowed (except hidden
// entries such as the manifest), so the AI can rediscover installed tools
// without asking. Writes and sandbox-run-script still require a grant.
// The tool's final files belong in its tool folder, not in the workspace.
namespace self_made_tools {

// The action object registered in the main action catalog.
const action::Action* tool();

// Directory that contains all self-made tools.
std::string rootPath();

// Restores the grants saved in <root>/.grants. Call once at startup. Missing
// or malformed entries are ignored. If no manifest exists yet, existing tool
// folders are adopted and the manifest is written.
void loadGrants();

// Returns the longest granted tool directory containing
// `canonicalPath`, or an empty string. `canonicalPath` must already be
// resolved with realpath().
std::string grantedRoot(const std::string& canonicalPath);

struct Pending {
    bool active = false;
    std::string op;
    std::string name;
    std::string explanation;
    unsigned long long id = 0;
};

// Main-thread side. While a request is pending the worker is blocked inside
// tool(); the main thread shows the approval prompt and calls decide().
Pending pending();
bool hasPending();
// Returns false when the request is no longer pending (for example after a
// timeout), so the caller does not report a decision that was not applied.
bool decide(bool allow);
void cancel(const std::string& reason);

} // namespace self_made_tools
