#pragma once

#include <string>
#include <vector>

// Platform-neutral interface for running an AI-written tool inside a real
// sandbox. The workspace write confinement in action.cpp is not this: this
// interface is for the execution layer that will run scripts.
//
// Backends are fail-closed: if the platform has no usable sandbox, available()
// returns false and run() never starts a process. The Linux backend uses
// bubblewrap; the Windows backend slot is reserved for an AppContainer/Job
// Object implementation.
namespace sandbox {

struct Limits {
    // Wall-clock budget for the whole sandboxed process tree.
    long long wallMs = 15000;
    // 0 disables the corresponding rlimit.
    size_t memoryBytes = 512u * 1024 * 1024;
    size_t maxFileBytes = 16u * 1024 * 1024;
    size_t maxOpenFiles = 128;
    // Captured stdout+stderr is truncated at this size.
    size_t maxOutputBytes = 64u * 1024;
};

struct Request {
    // argv[0] is the executable. System executables should use their usual
    // paths (for example /bin/sh or /usr/bin/python3). Scripts should be
    // relative to `workdir`, which the backend makes the sandbox's current
    // directory and exports as AI_CHAT_WORKSPACE.
    std::vector<std::string> command;
    // Existing host directory that becomes the sandbox's writable workdir.
    std::string workdir;
    Limits limits;
};

struct Result {
    bool ok = false;         // command ran and exited with status 0
    bool started = false;    // the backend actually started the command
    bool timedOut = false;
    bool truncated = false;  // captured output hit maxOutputBytes
    int exitCode = -1;       // exit status, or 128 + signal number
    std::string output;      // combined stdout and stderr
    std::string error;       // backend/startup failure
};

class Backend {
  public:
    virtual ~Backend() = default;

    // Stable, short name such as "bubblewrap" or "windows-appcontainer".
    virtual const char* name() const = 0;

    // True when this platform can run commands. On failure, `reason` explains
    // what is missing or not implemented.
    virtual bool available(std::string& reason) const = 0;

    // Runs `request.command` inside the sandbox. Never throws. If the sandbox
    // is unavailable the result has started=false and a human-readable error.
    virtual Result run(const Request& request) = 0;
};

// The backend selected for this platform. Never null.
Backend& platform();

} // namespace sandbox
