#pragma once

#include <string>

std::string trim(const std::string& s);
bool startsWith(const std::string& s, const char* p);

// True when the message explicitly asks the AI to wait for the user without
// a timer ("wait for me", "no timer", "this will take a while", ...).
bool userRequestsUntimedWait(const std::string& line);

// True when the message explicitly gives a waiting time ("wait 5 minutes",
// "I'll be back in 10 minutes", "wait until 15:30", ...). This lets the AI
// set a timer outside the normal 30-180 second range.
bool userRequestsExactTime(const std::string& line);

// True when the message clearly asks the AI to resume a paused autonomous
// loop ("continue", "go on", "keep going", ...). A bare "stop" or "don't"
// cancels the match.
bool userRequestsContinue(const std::string& line);
