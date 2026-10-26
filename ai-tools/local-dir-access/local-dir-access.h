#pragma once

#include <string>

#include "aichat/core/action.h"

// The local-dir-access tool lets the autonomous worker ask the user for
// read-only access to a directory outside its normal readable roots. Access
// is granted per directory and lasts only for the current ai-chat session.
namespace local_dir_access {

// The action object registered in the main action catalog.
const action::Action* tool();

// Worker side. Canonicalizes `raw`, asks the main thread for approval, and
// blocks until the user answers, the request times out, or the request is
// cancelled. Returns true on approval and stores the canonical path in
// `resolved`; otherwise stores a human-readable reason in `error`.
bool request(const std::string& raw, std::string& resolved, std::string& error);

struct Pending {
    bool active = false;
    std::string path;
    unsigned long long id = 0;
};

// Main-thread side.
Pending pending();
bool hasPending();
// Returns false when the request is no longer pending (for example after a
// timeout), so the caller does not report a decision that was not applied.
bool decide(bool allow);
void cancel(const std::string& reason);

// Returns the longest session-approved root containing `canonicalPath`, or an
// empty string. `canonicalPath` must already be resolved with realpath().
std::string grantedRoot(const std::string& canonicalPath);

} // namespace local_dir_access
