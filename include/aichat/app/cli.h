#pragma once

#include <string>

#include "aichat/core/config.h"

// What the command line asked the program to do.
enum class Action { Run, Help, SelfTest, MigrateExperience, Error };

struct Options {
    bool reset = false;
    bool test = false; // read-only memory for this session
    bool migrateDryRun = false;

    bool hasApiKey = false;
    bool hasBaseUrl = false;
    bool hasModel = false;
    bool hasNick = false;
    bool hasTimeout = false;
    bool hasLoginId = false;

    std::string apiKey;
    std::string baseUrl;
    std::string model;
    std::string nick;
    std::string loginId;
    int timeout = 0;
};

// Parses argv. Prints the error itself when it returns Action::Error.
Action parseArgs(int argc, char** argv, Options& opt);

// Applies the command-line overrides (and --reset) on top of the saved config.
void applyOptions(Config& cfg, const Options& opt);
