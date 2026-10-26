#include "aichat/core/facts/verifier.h"

#include <string>

#include <sys/stat.h>

namespace facts {

namespace {

bool statPath(const std::string& path, struct stat& st) {
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

void contradict(Fact& f, const std::string& why) {
    f.status = FactStatus::Contradicted;
    f.value = why;
    f.check = "stat";
}

} // namespace

void verify(std::vector<Fact>& list) {
    for (Fact& f : list) {
        switch (f.kind) {
        case FactKind::FsFile: {
            struct stat st;
            if (!statPath(f.subject, st)) {
                contradict(f, "missing: " + f.subject);
            } else if (!S_ISREG(st.st_mode)) {
                contradict(f, "not a regular file: " + f.subject);
            } else {
                f.status = FactStatus::Verified;
                f.check = "stat";
                f.value = "exists, " +
                          std::to_string(static_cast<long long>(st.st_size)) +
                          " bytes";
            }
            break;
        }
        case FactKind::FsDir: {
            struct stat st;
            if (!statPath(f.subject, st)) {
                contradict(f, "missing: " + f.subject);
            } else if (!S_ISDIR(st.st_mode)) {
                contradict(f, "not a directory: " + f.subject);
            } else {
                f.status = FactStatus::Verified;
                f.check = "stat";
                f.value = "exists as a directory";
            }
            break;
        }
        case FactKind::Wrote: {
            struct stat st;
            if (!statPath(f.subject, st))
                contradict(f, "missing after write: " + f.subject);
            else {
                f.status = FactStatus::Verified;
                f.check = "stat";
                f.value = "exists, " +
                          std::to_string(static_cast<long long>(st.st_size)) +
                          " bytes";
            }
            break;
        }
        case FactKind::Mirror: {
            struct stat st;
            if (!statPath(f.subject, st))
                contradict(f, "missing mirror: " + f.subject);
            else {
                f.status = FactStatus::Verified;
                f.check = "stat";
                f.value += "; verified on disk";
            }
            break;
        }
        case FactKind::Tool: {
            struct stat st;
            if (f.subject.empty()) break;
            if (!statPath(f.subject, st)) {
                contradict(f, "missing tool folder: " + f.subject);
            } else if (!S_ISDIR(st.st_mode)) {
                contradict(f, "not a directory: " + f.subject);
            } else {
                f.status = FactStatus::Verified;
                f.check = "stat";
            }
            break;
        }
        case FactKind::Run:
            // A runner fact carries the waitpid exit status; a parsed one
            // stays Reported.
            if (f.check == "waitpid" || f.check == "action")
                f.status = FactStatus::Verified;
            break;
        case FactKind::Grant:
            if (f.check == "action") f.status = FactStatus::Verified;
            break;
        case FactKind::Listing:
            // A listing produced by the action's own readdir is app-checked;
            // one parsed from text stays Reported.
            if (f.check == "parsed") f.status = FactStatus::Reported;
            break;
        default:
            break;
        }
    }
}

} // namespace facts
