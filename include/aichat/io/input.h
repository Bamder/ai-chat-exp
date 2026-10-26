#pragma once

#include <functional>
#include <string>

enum class Turn {
    Line,
    Timeout,
    Eof,
    // The background tick returned false (for example, autonomous work
    // finished while the user was silent).
    Background
};

// Reads one line, showing a live countdown. timeoutSec <= 0 = wait forever.
// Typing/editing pauses the countdown; one second of inactivity resumes it,
// unless the message still ends with the "@action-think" marker.
// When `tick` is set, it is called about once a second while the prompt is
// idle; returning false stops the read and yields Turn::Background. On a pipe
// the callback can only be checked before the blocking read. `liveStatus`
// supplies the short background-work summary shown in the prompt (for
// example "cycle 3 · acting · 400 tokens · energy 985.00/1000.00").
Turn readTurn(int timeoutSec, std::string& line,
              const std::function<bool()>& tick = {},
              const std::function<std::string()>& liveStatus = {});

// True when the message being edited ends with the user-side "@action-think"
// marker (trailing spaces and tabs are ignored). The marker means the user is
// still thinking, so the countdown must stay paused until the message changes.
bool endsWithThinkMarker(const std::string& message);

// True when the message contains the user-side "@action-energy-potion" marker
// as a standalone word. When found, the marker is removed from `message`
// together with the whitespace around it, so the text sent to the model stays
// clean. A marker attached to a word ("note@action-energy-potion") is ignored.
bool takeEnergyPotionMarker(std::string& message);

// Reads one line in the terminal's normal (canonical) mode.
// Returns false on EOF.
bool readLine(std::string& out);

// Reads one line without echoing it, printing a '*' for every character
// typed. Returns false on EOF.
bool readSecret(std::string& out);
