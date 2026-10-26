#pragma once

#include <map>
#include <string>
#include <vector>

#include "aichat/core/facts/fact.h"

// The autonomous loop talks to the outside world only through this small
// interface. Today every action is local (and sandboxed); in the future an
// implementation can wrap a tool call, an HTTP request, or a remote agent
// without changing the loop.
namespace action {

struct Request {
    std::string name;
    std::map<std::string, std::string> args;
};

struct Result {
    bool ok = false;
    std::string output;
    // Structured claims reported by the action itself. The fact engine
    // passes them to the verifier; an action that leaves this empty gets a
    // generic fact from the fallback layer.
    std::vector<facts::Fact> facts;
};

class Action {
  public:
    virtual ~Action() = default;

    virtual const char* name() const = 0;
    // One line for the model's action catalog.
    virtual const char* usage() const = 0;
    // Runs the action. Implementations must not throw; return a failed
    // Result with a human-readable message instead.
    virtual Result run(const std::map<std::string, std::string>& args) const = 0;
};

// The catalog offered to the model in autonomous prompts.
std::string catalog();

// Sets the experience target that lesson() records into. The autonomous
// worker sets this before executing an action; there is no lesson target in
// ordinary chat, so the action refuses rather than guessing one.
void setLessonTarget(const std::string& target);

// Executes a request by name. Unknown actions and missing arguments produce a
// failed Result.
Result execute(const Request& request);

// The only directory the AI may write to. Created on demand with mode 0700.
std::string workspacePath();

// Resolves an existing readable path using the same rules as read_file.
// Returns an empty string when the path is not readable.
std::string resolveReadablePath(const std::string& raw);

// Explains why resolveReadablePath() returned an empty string: either the
// path does not exist, or it exists but is outside the readable roots.
// Returns an empty string when the path is readable.
std::string readablePathProblem(const std::string& raw);

// For a mirror apply-back approved by the user: returns the allowed external
// root (the launch directory or a local_dir_access grant) containing
// `canonicalPath`, or an empty string when the path is not eligible. The
// config directory and hidden components are refused.
std::string approvedMirrorWriteRoot(const std::string& canonicalPath);

} // namespace action
