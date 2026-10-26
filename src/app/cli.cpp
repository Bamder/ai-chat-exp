#include "aichat/app/cli.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

#include "aichat/core/config.h"
#include "aichat/core/util.h"

Action parseArgs(int argc, char** argv, Options& opt) {
    Action action = Action::Run;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what, std::string& out) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ai-chat: %s needs a value\n", what);
                return false;
            }
            out = argv[++i];
            return true;
        };

        if      (a == "--help" || a == "-h") return Action::Help;
        else if (a == "--selftest")          return Action::SelfTest;
        else if (a == "--migrate-experience") action = Action::MigrateExperience;
        else if (a == "--dry-run")           { opt.migrateDryRun = true; }
        else if (a == "--test")              { opt.test = true; }
        else if (a == "--reset")             { opt.reset = true; }
        else if (a == "--api-key") {
            if (!next("--api-key", opt.apiKey)) return Action::Error;
            opt.hasApiKey = true;
        }
        else if (a == "--base-url") {
            if (!next("--base-url", opt.baseUrl)) return Action::Error;
            opt.hasBaseUrl = true;
        }
        else if (a == "--model") {
            if (!next("--model", opt.model)) return Action::Error;
            opt.hasModel = true;
        }
        else if (a == "--nick") {
            if (!next("--nick", opt.nick)) return Action::Error;
            opt.hasNick = true;
        }
        else if (a == "--timeout") {
            std::string v;
            if (!next("--timeout", v)) return Action::Error;
            opt.timeout = atoi(v.c_str());
            opt.hasTimeout = true;
        }
        else if (a == "--login-id" || a == "--login") {
            std::string v;
            if (!next("--login-id", v)) return Action::Error;
            const std::string clean = normalizeLoginId(v);
            if (clean.empty()) {
                std::fprintf(stderr,
                             "ai-chat: invalid login-id %s (use letters, "
                             "digits, '.', '_', '-' or '@', at most 64 "
                             "bytes)\n",
                             v.c_str());
                return Action::Error;
            }
            opt.loginId = clean;
            opt.hasLoginId = true;
        }
        else {
            std::fprintf(stderr, "ai-chat: unknown option %s (try --help)\n",
                         a.c_str());
            return Action::Error;
        }
    }
    return action;
}

void applyOptions(Config& cfg, const Options& opt) {
    if (opt.reset) {
        ::unlink(configPath().c_str());
        cfg = Config();
    }
    if (opt.hasBaseUrl) cfg.base_url = opt.baseUrl;
    if (opt.hasModel)   cfg.model    = opt.model;
    if (opt.hasNick && !opt.nick.empty()) cfg.nick = opt.nick;
    if (opt.hasTimeout && opt.timeout >= 0) cfg.timeout = opt.timeout;
    if (opt.hasLoginId) cfg.login_id = opt.loginId;
    else {
        const char* id = getenv("AI_CHAT_LOGIN_ID");
        const std::string clean = id ? normalizeLoginId(id) : std::string();
        if (!clean.empty()) cfg.login_id = clean;
    }
    if (opt.hasApiKey)  cfg.api_key  = opt.apiKey;
    else if (cfg.api_key.empty()) {
        const char* e = getenv("OPENAI_API_KEY");
        if (e && *e) cfg.api_key = trim(e);
    }
}
