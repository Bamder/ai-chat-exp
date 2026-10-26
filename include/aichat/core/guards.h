#pragma once

#include <map>
#include <string>
#include <vector>

#include "aichat/core/facts/store.h"

// Loop guards are not a controller. They never choose, rewrite or score a
// plan. They enforce only the invariants that verified facts cannot enforce
// by themselves:
//
//   - do not repeat an action that just changed nothing;
//   - do not accept "done" without evidence the fact store can verify;
//   - do not accept "give up" while a verified artifact exists.
//
// Everything else stays with the model.
namespace guards {

struct Verdict {
    bool allow = true;
    bool fatal = false;
    std::string reason;
};

// Watches for a no-op repeat. `observe` is called after an action runs;
// `check` before the next one.
class RepeatGuard {
  public:
    void observe(const std::string& action,
                 const std::map<std::string, std::string>& args,
                 int factVersion);

    Verdict check(const std::string& action,
                  const std::map<std::string, std::string>& args,
                  int factVersion);

  private:
    std::string key_;
    int version_ = -1;
    int strikes_ = 0;
};

// Reads the evidence keys from a <done evidence="key1, key2"/> tag. Returns
// an empty list for a bare <done/>.
std::vector<std::string> doneEvidence(const std::string& reply);

// A done claim is accepted only when:
//   - evidence keys are given, and each names a current fact that is
//     Verified (a run must also have exit=0); or
//   - no evidence is given but the target has no declared product, or has
//     verified output, or never used a tool (conversational target).
// The model chooses what proves completion; the store checks whether that
// proof exists.
Verdict checkDone(const facts::Store& store,
                  const std::vector<std::string>& evidence);

// Refuse giving up once while verified output exists, so the model has to
// look at the product it already has.
Verdict checkGiveUp(const facts::Store& store);

} // namespace guards
