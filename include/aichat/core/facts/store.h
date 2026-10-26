#pragma once

#include <map>
#include <string>
#include <vector>

#include "aichat/core/action.h"
#include "aichat/core/facts/fact.h"

// Layer 3: state.
//
// The single shared record of what is currently true. apply() runs the
// fallback and verifier, merges the result (superseding changed facts) and
// saves. render() produces the block every prompt reads.
//
// The store never decides what to do; it only says what is true. The loop
// guards (done / repeat / give-up, in guards.h) are a separate, thin consumer.
namespace facts {

class Store {
  public:
    // `storagePath` defaults to the config directory; tests pass a temp path.
    explicit Store(std::string storagePath = defaultPath());

    // Reads the store without starting a target. Used to archive the previous
    // task before a new target resets the store.
    void load();

    void begin(const std::string& target, const std::string& doneWhen);
    void apply(int cycle, const std::string& action,
               const std::map<std::string, std::string>& args,
               const action::Result& result);
    void rejected(int cycle, const std::string& action,
                  const std::map<std::string, std::string>& args,
                  const std::string& reason);

    std::string render() const;
    bool save() const;

    static std::string defaultPath();

    const std::string& target() const { return target_; }
    const std::string& doneWhen() const { return doneWhen_; }
    int revision() const { return revision_; }

    // Bumped only when a fact is added or its value/status/kind changes.
    // A repeated action that changes nothing leaves this unchanged, which is
    // what the repeat guard watches.
    int version() const { return changeVersion_; }

    // True when a verified artifact exists: a successful run, a write, a
    // mirror, or an installed tool. Inputs such as grants and listings do not
    // count.
    bool hasVerifiedOutput() const;

    // True when the store holds any fact at all. A target that never touched
    // a tool has no facts; that is the conversational escape hatch.
    bool hasFacts() const { return !facts_.empty(); }

    // Returns the current fact with this key, or nullptr.
    const Fact* find(const std::string& key) const;

    // Facts removed from the active store because it exceeded kMaxFacts or
    // kMaxSuperseded. The worker drains this after an action and archives it
    // in the target's experience log.
    std::vector<Fact> takeEvicted();

  private:
    void reset();
    void merge(Fact fact);

    std::string storage_;
    std::string target_;
    std::string doneWhen_;
    int revision_ = 0;
    int lastCycle_ = 0;
    int changeVersion_ = 0;
    std::vector<Fact> facts_;
    std::vector<Fact> superseded_;
    std::vector<Fact> evicted_;
};

} // namespace facts
