#pragma once

#include <string>

#include "aichat/core/action.h"

// The sandbox mirror tools copy readable files or directories from outside
// the sandbox into the workspace, where the AI can inspect, edit or run them.
// Applying a changed mirror back to the host always requires user approval;
// the user chooses replace, new, or not to apply.
namespace sandbox_mirror {

// The action objects registered in the main action catalog.
const action::Action* fileTool();
const action::Action* directoryTool();

enum class ApplyChoice {
    Deny,
    Replace,
    New,
};

struct Pending {
    bool active = false;
    bool directory = false;
    std::string origin;
    std::string mirror;
    std::string replaceDestination;
    std::string newDestination;
    std::string explanation;
    unsigned long long id = 0;
};

// Main-thread side. While a request is pending the worker is blocked inside
// the mirror action; the main thread shows the prompt and calls decide().
Pending pending();
bool hasPending();
// Returns false when the request is no longer pending (for example after a
// timeout), so the caller does not report a decision that was not applied.
bool decide(ApplyChoice choice);
void cancel(const std::string& reason);

} // namespace sandbox_mirror
