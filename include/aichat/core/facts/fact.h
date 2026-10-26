#pragma once

#include <string>
#include <utility>

// Domain types for the fact engine. A Fact is a single claim about the
// world, made by an action and (where possible) checked by the app.
//
// Layering:
//   action::Result  ->  fallback()  ->  verify()  ->  Store  ->  render()
//                       claims        checks       state      prompt text
namespace facts {

enum class FactKind {
    Generic,    // an unclassified result
    FsFile,     // a regular file exists
    FsDir,      // a directory exists
    Listing,    // the entries of a directory
    Wrote,      // a file was written or applied
    Run,        // a script ran (exit code, timeout, output size)
    Grant,      // read access to a directory is active
    Tool,       // a self-made tool folder is installed
    Mirror,     // a mirrored artifact
    Rejection,  // a planned step was refused before running
    Failure,    // an action failed
};

enum class FactStatus {
    Verified,    // checked by the app (stat, waitpid, action state)
    Reported,    // taken from action output, not independently checked
    Contradicted,// the app checked and the claim is false
    Superseded,  // replaced by a newer fact with the same key
};

struct Fact {
    std::string key;      // stable identity, e.g. "file:/path/x.pdf"
    std::string subject;  // path or name the fact is about
    std::string value;    // compact, human-readable statement
    FactKind kind = FactKind::Generic;
    FactStatus status = FactStatus::Reported;
    std::string source;   // action that produced it
    std::string check;    // "stat", "waitpid", "action", "parsed", ...
    int cycle = 0;
};

inline const char* kindName(FactKind kind) {
    switch (kind) {
    case FactKind::Generic: return "generic";
    case FactKind::FsFile: return "file";
    case FactKind::FsDir: return "dir";
    case FactKind::Listing: return "listing";
    case FactKind::Wrote: return "wrote";
    case FactKind::Run: return "run";
    case FactKind::Grant: return "grant";
    case FactKind::Tool: return "tool";
    case FactKind::Mirror: return "mirror";
    case FactKind::Rejection: return "rejection";
    case FactKind::Failure: return "failure";
    }
    return "generic";
}

inline const char* statusName(FactStatus status) {
    switch (status) {
    case FactStatus::Verified: return "verified";
    case FactStatus::Reported: return "reported";
    case FactStatus::Contradicted: return "contradicted";
    case FactStatus::Superseded: return "superseded";
    }
    return "reported";
}

// Convenience constructor used by actions that report their own structured
// facts. The cycle and source are filled in by fallback() when missing.
inline Fact make(FactKind kind, std::string key, std::string subject,
                 std::string value,
                 FactStatus status = FactStatus::Reported,
                 std::string check = "reported") {
    Fact f;
    f.kind = kind;
    f.key = std::move(key);
    f.subject = std::move(subject);
    f.value = std::move(value);
    f.status = status;
    f.check = std::move(check);
    return f;
}

} // namespace facts
