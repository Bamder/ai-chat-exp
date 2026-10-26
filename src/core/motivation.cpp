#include "aichat/core/motivation.h"

#include <algorithm>
#include <sstream>

namespace motivation {

void State::progress(int amount) {
    if (amount <= 0) return;
    value = std::min(100, value + amount);
    noProgress = 0;
    ++goodStreak;
}

void State::setback(int amount) {
    if (amount <= 0) return;
    value = std::max(0, value - amount);
    ++noProgress;
    goodStreak = 0;
}

void State::decay() {
    if (value > 50)
        --value;
    else if (value < 50)
        ++value;
}

void State::factMade() { noFactStreak = 0; }

void State::noFact() {
    ++noFactStreak;
    value = std::max(0, value - 3);
    ++noProgress;
    goodStreak = 0;
}

bool State::encourages(unsigned seed) const {
    // Small deterministic mixer; avoids depending on std::hash, which is
    // implementation-specific, so tests and prompt runs are stable.
    unsigned x = seed + 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<int>(x % 100u) < value;
}

std::string State::render(unsigned seed) const {
    std::ostringstream out;
    out << "Motivation for this target: " << value << "/100\n"
        << "  attempts without real progress: " << noProgress << "\n"
        << "  good steps in a row: " << goodStreak << "\n";
    if (encourages(seed)) {
        out << "  You feel motivated. Continue with this approach and build "
               "on what is working.\n";
    } else {
        out << "  You feel discouraged. Question the current approach and "
               "consider changing it.\n";
    }
    // A deterministic stall signal, separate from the seeded mood above.
    // It counts executed steps that produced no new fact, so the observer
    // calling a repetitive read "small progress" cannot erase it. The loop
    // does not refuse a step for being unproductive; it says so and lets the
    // planner decide.
    if (noFactStreak >= 3) {
        out << "  Stall signal: " << noFactStreak
            << " steps in a row ran without producing a new fact. The same "
               "kind of step is being repeated. Do not run it again with a "
               "different argument; choose a different action from the "
               "catalog, or state plainly what is blocking you. If the "
               "information is already in the fact store above, use it "
               "instead of collecting it again.\n";
    }
    return out.str();
}

std::string State::line(const std::string& why) const {
    std::ostringstream out;
    out << "motivation: " << value << "/100";
    if (!why.empty()) out << " (" << why << ")";
    return out.str();
}

State& Store::forTarget(const std::string& key) { return states_[key]; }

const State& Store::forTarget(const std::string& key) const {
    const auto it = states_.find(key);
    if (it != states_.end()) return it->second;
    static const State neutral;
    return neutral;
}

} // namespace motivation
