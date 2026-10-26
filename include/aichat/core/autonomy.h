#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aichat/core/chat.h"
#include "aichat/core/config.h"

// A consistent copy of the chat state, taken by the worker on a resume.
struct AutonomySnapshot {
    std::vector<Message> history;
    std::string summary; // rendered documentary memory of earlier periods
    std::string sca;
};

struct AutonomyResult {
    bool ok = false;      // the loop ran to an orderly stop
    bool tired = false;   // it stopped because energy ran out
    bool gaveUp = false;  // the model abandoned the target
    bool targetChanged = false;
    bool cognitionUpdated = false;
    int steps = 0;
    double energyLeft = 0.0;
    std::string reason;
    std::string summary;     // one short message for the chat transcript
    std::string target;      // target the session worked on
    std::string outcome;     // completed, gave up, ran out of energy, ...
    std::string reflection;  // one-line reflection for the end-of-session display
    std::string newSca;      // set when the self-cognition file was refreshed
};

// A snapshot of the worker's public state. The UI uses the fields instead of
// parsing statusText(), so the human-readable status and the prompt can share
// the same source.
struct AutonomyStatus {
    bool active = false;
    bool animating = false; // making progress; the UI may rotate a spinner
    std::string state;      // running, paused, finishing
    std::string phase;      // analysing, planning, acting, ...
    std::string target;
    std::string last;
    int cycle = 0;
    double energy = 0.0;
    double energyFull = 1000.0;
    double cost = 0.0;           // token cost of the current step so far
    long long tokens = 0;        // tokens used by the current step so far
};

// One parsed `<action name="...">...</action>` request.
struct ActionSpec {
    std::string name;
    std::map<std::string, std::string> args;
};

// Parses an action block. Arguments use
//   <arg name="path">books/one.txt</arg>
// Self-closing tags with plain attributes are also accepted for convenience.
bool parseAction(const std::string& text, ActionSpec& out);

// The preview checker may only approve or reject a planned step. A reply
// without a usable verdict is Unclear; the loop fails closed and does not run
// the step. A failed preview request is not an approval.
enum class PreviewVerdict { Approve, Reject, Unclear };

// Reads the checker's reply. On Reject, `reason` receives its one-line
// explanation (empty when it gave none).
PreviewVerdict previewVerdict(const std::string& reply, std::string& reason);

// Rounds `value` to two decimal places using banker's rounding (half to
// even). Used for token-based energy costs.
double bankersRound2(double value);

// The energy cost of `tokens` model tokens in cycle `cycle`:
//
//   (tokens / 5000) * (1 + 0.1 * cycle^1.15)
//
// rounded half-to-even to two decimals. A non-positive token count or cycle
// costs nothing.
double energyCostForTokens(long long tokens, int cycle);

// Formats an energy amount or cost as a float with exactly two decimal
// places, rounded half-to-even ("1000.00", "998.75", "2.10").
std::string amountText(double value);

// The AI's energy budget. It trickles back at 5 points per minute while no
// autonomous loop is active. All methods are thread-safe.
class Energy {
  public:
    explicit Energy(double full = 1000.0);

    double peek() const;
    // Accrues elapsed minutes (if not in a loop) and returns the value.
    double available();
    void beginLoop();
    void endLoop();
    // Fills the budget to its maximum immediately (an energy potion).
    void refill();
    // Deducts `cost`. Returns false and leaves the value untouched when there
    // is not enough energy.
    bool spend(double cost, double& left);
    // Deducts `cost` even when it exceeds the budget: the value stops at zero
    // and false is returned. Token costs are charged after the tokens were
    // already used, so the budget must not go negative.
    bool charge(double cost, double& left);

  private:
    void accrueLocked();

    mutable std::mutex mutex_;
    double value_;
    double full_;
    bool inLoop_ = false;
    std::chrono::steady_clock::time_point last_;
};

// Owns the background worker thread. The chat thread keeps the terminal and
// the conversation history; the worker only reads snapshots and writes the
// .sca file atomically at the end of a session.
class AutonomyController {
  public:
    using SnapshotFn = std::function<AutonomySnapshot()>;

    AutonomyController();
    ~AutonomyController();

    AutonomyController(const AutonomyController&) = delete;
    AutonomyController& operator=(const AutonomyController&) = delete;

    // Starts a session. Returns false when a loop already exists (running,
    // paused, or still awaiting takeResult()), enforcing one loop at a time.
    bool start(const Config& cfg, bool testMode, SnapshotFn snapshot);

    // Signals the worker and waits for it to exit. Discards its result.
    void stop();

    // Asks the worker to pause and blocks until it acknowledges. Safe to call
    // when the worker has already finished.
    void pause();

    // Lets a paused worker continue. The worker reloads .sca and checks the
    // short-term target before doing any more work.
    void resume();

    // True when a loop exists in any form: running, paused, or with a
    // result still waiting to be collected. This is the single guard that
    // enforces "at most one autonomous loop at a time".
    bool busy() const;
    bool running() const;   // actively working
    bool finished() const;  // a result is waiting for takeResult()
    bool paused() const;

    // Status lines queued by the worker. The main thread prints them; this
    // keeps terminal output single-threaded.
    std::vector<std::string> takeMessages();

    // Returns the result of a finished session and joins the worker.
    AutonomyResult takeResult();

    // A compact description for the chat system prompt.
    std::string statusText() const;

    // The same state as a structure, for structured UI output.
    AutonomyStatus status() const;

    // A very short line for the live input prompt: cycle, phase and energy.
    std::string liveStatus() const;

    double energyLeft() const;

    // Restores the energy budget to full immediately. Safe to call while a
    // loop is running, paused or idle.
    void refillEnergy();

  private:
    enum class State { Inactive, Running, Paused, Finished };
    enum class Check { Continue, Stop, Resumed };

    void run();
    Check checkpoint(std::string& scaAtPause);
    void log(const std::string& line);
    void setStatus(const std::string& phase, const std::string& target,
                   const std::string& last, int step, double energy);
    void setStepCost(double cost, double energy, long long tokens);
    AutonomyResult work();
    bool updateCognition(const Config& cfg, const std::string& reflection,
                         const std::string& factText,
                         const std::string& journal, AutonomyResult& result);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    mutable std::mutex messageMutex_;

    std::thread thread_;
    State state_ = State::Inactive;
    bool stop_ = false;
    bool discard_ = false;
    bool pauseRequested_ = false;

    Config cfg_;
    bool testMode_ = false;
    SnapshotFn snapshot_;

    AutonomyResult result_;
    std::vector<std::string> messages_;

    double energyLeft_ = 1000.0;
    double cost_ = 0.0;
    long long tokens_ = 0;
    int step_ = 0;
    std::string phase_;
    std::string target_;
    std::string last_;
    std::atomic<long long> lastActivityMs_{0};

    mutable Energy energy_;
};
