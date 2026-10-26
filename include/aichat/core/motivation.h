#pragma once

#include <map>
#include <string>

// A target-scoped motivation signal for the autonomous loop. It never blocks
// a step: it turns verified progress and dead ends into a 0..100 value that
// the planner, preview and observer can read. A high value biases the prompt
// toward "keep going"; a low value biases it toward "question this approach".
// The caller owns one State per target, so a different target starts from a
// neutral value instead of inheriting the previous target's mood.
namespace motivation {

struct State {
    int value = 50;      // 0..100, 50 is neutral
    int noProgress = 0;  // consecutive attempts without real progress
    int goodStreak = 0;  // consecutive attempts with real progress
    // Consecutive executed steps that produced no new fact. This is the
    // mechanical stall signal: it does not depend on the observer's opinion
    // of progress, so "small progress" cannot reset it.
    int noFactStreak = 0;

    // Reward a step that produced verified progress.
    void progress(int amount);
    // Punish a failed attempt, a rejected plan, or a step judged as no
    // progress.
    void setback(int amount);
    // Drift one point back toward neutral (50) per cycle.
    void decay();

    // A tool produced a new or changed fact.
    void factMade();
    // A step ran successfully but changed no fact (the same read, listing or
    // thought). Counts toward the stall signal and lowers the mood slightly.
    void noFact();
    int stallStreak() const { return noFactStreak; }

    // Deterministic pseudo-random draw seeded by the caller. True with
    // probability value/100, so higher motivation is more likely to
    // encourage the current approach. The same seed always yields the same
    // answer, which keeps one cycle's prompts consistent.
    bool encourages(unsigned seed) const;

    // Prompt block with the value and the seeded guidance.
    std::string render(unsigned seed) const;
    // One-line log message, for example:
    //   motivation: 42/100 (step failed)
    std::string line(const std::string& why) const;
};

// Keeps one State per target. A different target gets a fresh neutral State,
// so old motivation cannot leak across targets.
class Store {
  public:
    State& forTarget(const std::string& key);
    const State& forTarget(const std::string& key) const;

  private:
    std::map<std::string, State> states_;
};

} // namespace motivation
