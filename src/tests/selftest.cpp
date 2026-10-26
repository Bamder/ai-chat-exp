#include "aichat/tests/selftest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <unistd.h>
#include <sys/stat.h>

#include "aichat/core/action.h"
#include "aichat/core/autonomy.h"
#include "aichat/core/chat.h"
#include "aichat/core/cognition.h"
#include "aichat/core/config.h"
#include "aichat/core/documentary.h"
#include "aichat/core/experience.h"
#include "aichat/core/story.h"
#include "aichat/core/facts/fallback.h"
#include "aichat/core/facts/store.h"
#include "aichat/core/facts/verifier.h"
#include "aichat/core/guards.h"
#include "aichat/core/identity.h"
#include "aichat/core/motivation.h"
#include "aichat/io/input.h"
#include "aichat/io/json.h"
#include "aichat/core/sandbox.h"
#include "aichat/core/sentence.h"
#include "aichat/core/util.h"
#include "aichat/ui/ui.h"
#include "aichat/core/tag_manuals.h"
#include "ai-tools/self-made-tool/self-made-tool.h"
#include "ai-tools/text-edit/text-edit.h"
#include "ai-tools/text-grep/text-grep.h"
#include "ai-tools/sse-watch/sse-watch.h"
#include "ai-tools/text-utf8/text-utf8.h"

int selfTest() {
    int fails = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++fails;
    };

    // Every state path is anchored on AI_CHAT_HOME. Give the whole test run a
    // scratch directory so it never reads or writes the real config.
    const char* oldHomeEnv = getenv("AI_CHAT_HOME");
    const std::string oldHome = oldHomeEnv ? oldHomeEnv : "";
    std::string scratchHome;
    ::mkdir("/tmp/opencode", 0700);
    char homeTemplate[] = "/tmp/opencode/selftest-home-XXXXXX";
    if (char* made = ::mkdtemp(homeTemplate)) {
        scratchHome = made;
        setenv("AI_CHAT_HOME", scratchHome.c_str(), 1);
    }
    check(!scratchHome.empty(),
          "creates a temporary AI_CHAT_HOME for the test run");

    if (!scratchHome.empty()) {
        std::string error;
        const std::string nested = scratchHome + "/nested/state";
        check(ensureAiChatHome(nested, error),
              "AI_CHAT_HOME is created recursively");
        check(::access((nested + "/.config/ai-chat").c_str(),
                       W_OK | X_OK) == 0,
              "the created AI_CHAT_HOME state is writable");
        check(!ensureAiChatHome("relative/path", error),
              "a relative AI_CHAT_HOME is refused");

        Config saved;
        saved.api_key = "test-key";
        saved.login_id = "alice";
        check(saveConfig(saved), "saves a login-id in the config");
        const Config restored = loadConfig();
        check(restored.login_id == "alice",
              "loads the saved login-id from the config");
    }

    check(jsonUnescape("a\\nb") == "a\nb", "unescape \\n");
    check(jsonUnescape("say \\\"hi\\\" \\\\ ok") == "say \"hi\" \\ ok",
          "unescape quotes/backslash");
    check(jsonUnescape("\\u00e9") == "\xc3\xa9", "unescape \\u00e9");
    check(jsonUnescape("\\u4f60") == "\xe4\xbd\xa0", "unescape \\u4f60 (CJK)");

    {
        const std::string cjk = "\xE4\xBD\xA0\xE5\xA5\xBD"; // 你好
        check(text_utf8::valid("plain ASCII"), "utf8: ASCII is valid");
        check(text_utf8::valid(cjk), "utf8: CJK is valid");
        check(text_utf8::valid("\xF0\x9F\x99\x82"), "utf8: emoji is valid");
        check(!text_utf8::valid("\xE4\xB8"), "utf8: truncated is invalid");
        check(!text_utf8::valid("\x80"), "utf8: lone continuation is invalid");
        check(!text_utf8::valid("\xED\xA0\x80"), "utf8: surrogate is invalid");
        check(!text_utf8::valid("\xC0\xAF"), "utf8: overlong is invalid");
        check(!text_utf8::valid("\xF4\x90\x80\x80"),
              "utf8: above U+10FFFF is invalid");

        bool cutsOk = true;
        const std::string mixed = "a" + cjk + "\xF0\x9F\x99\x82" + cjk;
        for (size_t n = 0; n <= mixed.size(); ++n) {
            if (!text_utf8::valid(text_utf8::head(mixed, n))) cutsOk = false;
            if (!text_utf8::valid(text_utf8::tail(mixed, n))) cutsOk = false;
        }
        check(cutsOk, "utf8: every byte cut stays on a code-point boundary");
        check(text_utf8::valid(text_utf8::tailLines(cjk + cjk + cjk, 5)),
              "utf8: tailLines stays valid without newlines");

        size_t repaired = 0;
        const std::string bad = text_utf8::sanitize("\xE4\xB8", &repaired);
        check(repaired == 1 && bad == "\xEF\xBF\xBD",
              "utf8: a truncated character becomes one U+FFFD");
        check(text_utf8::valid(text_utf8::sanitize("\x80\xFF\xED\xA0\x80")),
              "utf8: sanitize always returns valid UTF-8");
        check(text_utf8::sanitize("ok") == "ok",
              "utf8: sanitize keeps valid text unchanged");

        std::string everyByte;
        for (int i = 0; i < 256; ++i)
            everyByte += static_cast<char>(i);
        const std::string cleaned = text_utf8::sanitize(everyByte);
        bool fuzzOk = text_utf8::valid(cleaned);
        for (size_t n = 0; n <= cleaned.size(); ++n) {
            if (!text_utf8::valid(text_utf8::head(cleaned, n)) ||
                !text_utf8::valid(text_utf8::tail(cleaned, n)))
                fuzzOk = false;
        }
        check(fuzzOk, "utf8: every byte value survives sanitize and cuts");

        const std::string escaped = jsonEscape("\xE4\xB8");
        check(text_utf8::valid(escaped) &&
                  escaped.find("\xEF\xBF\xBD") != std::string::npos,
              "jsonEscape: bad UTF-8 becomes U+FFFD, not a broken body");

        // The shape that killed the autonomous loop: one huge line of CJK,
        // sliced by bytes for the journal, then put in a request body.
        std::string huge;
        huge.reserve(300000);
        for (int i = 0; i < 100000; ++i) huge += "\xE5\x9B\xBE"; // 图
        const std::string excerpt = text_utf8::tailLines(huge, 1500);
        const std::string body =
            "{\"content\":\"" + jsonEscape(excerpt) + "\"}";
        check(text_utf8::valid(body),
              "utf8: a 1500-byte CJK tail yields a valid request body");
    }

    {
        const std::string note =
            "line one\nline two\n=== page 8 ===\ntext with 6-22 in it\n";
        const text_grep::Result three = text_grep::find(note, "page", 0);
        check(three.ok && three.total == 1 && three.matches.size() == 1,
              "grep: finds one literal match");
        check(three.matches.size() == 1 && three.matches[0].line == 3 &&
                  three.matches[0].offset == note.find("page"),
              "grep: reports the correct line and byte offset");
        check(three.matches[0].text == "=== page 8 ===",
              "grep: returns the whole matching line");
        const text_grep::Result many = text_grep::find(note, "e", 2);
        check(many.total > 2 && many.matches.size() == 2,
              "grep: total counts matches beyond the display limit");
        check(!text_grep::find(note, "", 0).ok,
              "grep: an empty pattern is an error");
        const text_grep::Result none = text_grep::find(note, "zzz", 0);
        check(none.ok && none.total == 0, "grep: no match is not an error");
    }

    {
        const std::string src = "alpha\nbeta\ngamma\n";
        const text_edit::Result one =
            text_edit::replaceOnce(src, "beta", "BETA");
        check(one.ok && one.text == "alpha\nBETA\ngamma\n",
              "edit: replaces a unique old text");
        check(one.line == 2 && one.offset == 6,
              "edit: reports the replacement line and offset");
        check(one.text.size() == 17, "edit: keeps the rest of the file");

        const text_edit::Result missing =
            text_edit::replaceOnce(src, "delta", "x");
        check(!missing.ok && missing.text.empty(),
              "edit: missing old text is an error");

        const std::string twice = "x\ny\nx\n";
        const text_edit::Result ambiguous =
            text_edit::replaceOnce(twice, "x", "z");
        check(!ambiguous.ok && ambiguous.occurrences == 2,
              "edit: an ambiguous old text is refused, not guessed");

        const text_edit::Result del = text_edit::replaceOnce(src, "beta\n", "");
        check(del.ok && del.text == "alpha\ngamma\n",
              "edit: an empty new text deletes the old text");

        check(!text_edit::replaceOnce(src, "", "x").ok,
              "edit: an empty old text is an error");
    }

    {
        check(sse_watch::isTokenFrame(
                  "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}"),
              "sse: a content frame is token activity");
        check(sse_watch::isTokenFrame(
                  "data:{\"choices\":[{\"delta\":{\"reasoning_content\":\"hmm\"}}]}"),
              "sse: a reasoning frame is token activity");
        check(!sse_watch::isTokenFrame(": keep-alive"),
              "sse: a comment is not token activity");
        check(!sse_watch::isTokenFrame("event: ping"),
              "sse: an event line is not token activity");
        check(!sse_watch::isTokenFrame("data: [DONE]"),
              "sse: [DONE] is not token activity");
        check(!sse_watch::isTokenFrame(
                  "data: {\"choices\":[{\"delta\":{\"content\":\"\"}}]}"),
              "sse: an empty delta is not token activity");
        check(!sse_watch::isTokenFrame(
                  "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}"),
              "sse: a role-only frame is not token activity");
        check(!sse_watch::isTokenFrame("data: {}"),
              "sse: a heartbeat is not token activity");

        check(!sse_watch::stalled(100000, 0, false, 180000, 90000),
              "sse: the first-token budget allows a slow prefill");
        check(sse_watch::stalled(180001, 0, false, 180000, 90000),
              "sse: no first token past the budget is a stall");
        check(!sse_watch::stalled(90000, 0, true, 180000, 90000),
              "sse: a 90s token gap is still allowed");
        check(sse_watch::stalled(90001, 0, true, 180000, 90000),
              "sse: a gap over 90s is a stall");
        check(!sse_watch::stalled(1000, 0, true, 180000, 0),
              "sse: a zero gap limit disables the check");
    }

    {
        // End-to-end: a fake curl that keeps the stream alive with comments
        // and never sends a data frame. With a 1.5 s first-token budget the
        // watchdog must kill it and report Stalled without retrying.
        char dir[] = "/tmp/opencode/fakecurl-XXXXXX";
        if (char* made = ::mkdtemp(dir)) {
            const std::string script = std::string(made) + "/curl";
            {
                std::ofstream out(script);
                out << "#!/bin/sh\n";
                out << "printf ': keep-alive\\n\\n'\n";
                // exec: the shell becomes sleep, so the app's SIGKILL closes
                // the pipe instead of leaving an orphan holding it open.
                out << "exec sleep 30\n";
            }
            ::chmod(script.c_str(), 0700);

            const char* oldPathEnv = getenv("PATH");
            const std::string oldPath = oldPathEnv ? oldPathEnv : "";
            setenv("PATH", (std::string(made) + ":" + oldPath).c_str(), 1);
            setenv("AI_CHAT_FIRST_TOKEN_MS", "1500", 1);
            setenv("AI_CHAT_TOKEN_GAP_MS", "1500", 1);

            Config cfg;
            cfg.base_url = "http://127.0.0.1:1/v1";
            cfg.api_key = "test";
            cfg.model = "test";
            const ChatOutcome stalled =
                completeChatOutcome(cfg, {{"user", "hi"}});
            check(stalled.status == ChatStatus::Stalled,
                  "sse: a keep-alive-only stream is stopped as stalled");

            // A slow but progressing stream must survive: tokens arrive one
            // second apart, so the total exceeds the gap but no single gap
            // does.
            {
                std::ofstream out(script);
                out << "#!/bin/sh\n";
                out << "printf 'data: {\"choices\":[{\"delta\":{\"content\":"
                       "\"a\"}}]}\\n\\n'\n";
                out << "sleep 1\n";
                out << "printf 'data: {\"choices\":[{\"delta\":{\"content\":"
                       "\"b\"}}]}\\n\\n'\n";
                out << "sleep 1\n";
                out << "printf 'data: {\"choices\":[{\"delta\":{\"content\":"
                       "\"c\"}}]}\\n\\n'\n";
                out << "printf 'data: {\"choices\":[],\"usage\":{"
                       "\"prompt_tokens\":10,\"completion_tokens\":3,"
                       "\"total_tokens\":13}}\\n\\n'\n";
                out << "printf 'data: [DONE]\\n\\n'\n";
            }
            const ChatOutcome streamed =
                completeChatOutcome(cfg, {{"user", "hi"}});
            check(streamed.status == ChatStatus::Ok &&
                      streamed.text == "abc",
                  "sse: a slow but progressing stream is not killed");
            check(streamed.totalTokens == 13,
                  "sse: streamed usage reaches the chat outcome");

            unsetenv("AI_CHAT_FIRST_TOKEN_MS");
            unsetenv("AI_CHAT_TOKEN_GAP_MS");
            setenv("PATH", oldPath.c_str(), 1);
            std::remove(script.c_str());
            ::rmdir(made);
        }
    }

    check(endsWithThinkMarker("@action-think"),
          "detects the thinking marker");
    check(endsWithThinkMarker("let me think @action-think  "),
          "detects the thinking marker before trailing spaces");
    check(!endsWithThinkMarker("let me think @action-think more"),
          "ignores a marker that is no longer last");
    check(!endsWithThinkMarker("note@action-think"),
          "does not treat an attached marker as an action");
    check(!endsWithThinkMarker("let me think"), "no marker means no pause");

    {
        std::string message = "hello @action-energy-potion world";
        check(takeEnergyPotionMarker(message),
              "detects the energy potion marker in a message");
        check(message == "hello world",
              "removes the potion marker and the extra whitespace");

        message = "note@action-energy-potion";
        check(!takeEnergyPotionMarker(message),
              "does not treat an attached potion marker as an action");

        message = "@action-energy-potion";
        check(takeEnergyPotionMarker(message) && message.empty(),
              "accepts a message that is only the potion marker");

        message = "please @action-energy-potion!";
        check(takeEnergyPotionMarker(message) && message == "please!",
              "keeps punctuation attached to the surrounding text");
    }

    {
        std::string out;
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Hello! How are ");
        pr.feed("you?\nI am fine");
        pr.feed(". Bye ");
        pr.feed("see ya <wai");
        pr.feed("t/>");
        pr.finish();
        fclose(mem);
        out.assign(cap, capLen);
        free(cap);
        check(out == "Hello! How are you?\nI am fine. Bye see ya ",
              "streams text, holds back the <wait/> prefix");
        check(pr.wantsWait(), "detects the <wait/> tag");
        check(pr.text() == out, "history text has no tag");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Hel");
        std::string first(cap, capLen);
        pr.feed("lo <wai");
        std::string second(cap, capLen);
        pr.feed("t/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(first == "Hel", "prints a chunk immediately");
        check(second == "Hello ",
              "holds back a tail that could become a tag");
        check(out == "Hello ", "removes the tag and keeps the text");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("One. Two mor");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "One. Two mor", "flushes the unfinished last chunk");
        check(!pr.wantsWait(), "no false <wait/> detection");
        check(pr.timerSec() == 0, "no timer tag means the default is used");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Here you go. <timer 90/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "Here you go. ", "strips the <timer> tag");
        check(pr.timerSec() == 90, "reads the AI-chosen timer");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Take your time. <tim");
        pr.feed("er 12");
        pr.feed("0/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "Take your time. ", "holds back a split <timer> tag");
        check(pr.timerSec() == 120, "reads a split timer tag");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Quick.<timer 5/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "Quick.", "strips a small timer tag");
        check(pr.timerSec() == TIMER_MIN_SEC, "clamps the timer to 30s");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Later.<timer 999/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "Later.", "strips a large timer tag");
        check(pr.timerSec() == TIMER_MAX_SEC, "clamps the timer to 180s");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem, true);
        pr.feed("See you then.<timer 600/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "See you then.", "strips an exact-time long timer tag");
        check(pr.timerSec() == 600,
              "allows a timer longer than 180s after an exact-time request");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("I see it differently now. <sca/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "I see it differently now. ",
              "strips the <sca/> tag");
        check(pr.wantsCognition(),
              "detects the model's request to rewrite self-cognition");
        check(!pr.wantsWait(), "the <sca/> tag is not a wait request");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("A new goal. <sc");
        std::string first(cap, capLen);
        pr.feed("a/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(first == "A new goal. ",
              "holds back a split <sca/> tag");
        check(out == "A new goal. ", "removes a split <sca/> tag");
        check(pr.wantsCognition(), "reads a split <sca/> tag");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem, true);
        pr.feed("Wait.<timer 10/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "Wait.", "strips a short exact-time timer tag");
        check(pr.timerSec() == 10,
              "allows a timer shorter than 30s after an exact-time request");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem, true);
        pr.feed("Much later.<timer 999999/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "Much later.", "strips an oversized timer tag");
        check(pr.timerSec() == TIMER_HARD_MAX_SEC,
              "caps an exact-time timer at the hard limit");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Pi is 3.14 and e.g. fine. Really!");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(out == "Pi is 3.14 and e.g. fine. Really!",
              "does not split on decimals");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("I'll get to it. <aut");
        std::string first(cap, capLen);
        pr.feed("o/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(first == "I'll get to it. ",
              "holds back a split <auto/> tag");
        check(out == "I'll get to it. ", "removes a split <auto/> tag");
        check(pr.wantsAutonomy(), "detects the <auto/> tag");
        check(!pr.wantsCognition(), "the <auto/> tag is not an .sca request");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Noted. <lesson>PURPOSE: keep the rule\nPRACTICE: read the "
                "printed line\nEVIDENCE: this test");
        std::string first(cap, capLen);
        pr.feed(".</lesson>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(first == "Noted. ", "holds back a split <lesson> tag");
        check(out == "Noted. ", "removes a split <lesson> tag");
        check(pr.lessonRequests().size() == 1,
              "detects one <lesson> request");
        check(pr.lessonRequests()[0].find("read the printed line") !=
                  std::string::npos,
              "reads the <lesson> body");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Noted. <story object=\"book-androids\">WHO: Rick\nWANTED: "
                "the owl");
        std::string first(cap, capLen);
        pr.feed(".</story>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(first == "Noted. ", "holds back a split <story> tag");
        check(out == "Noted. ", "removes a split <story> tag");
        check(pr.storyRequests().size() == 1,
              "detects one <story> request");
        check(pr.storyRequests()[0].object == "book-androids" &&
                  pr.storyRequests()[0].body.find("WANTED: the owl") !=
                      std::string::npos,
              "reads the <story> object and body");
    }
    {
        char* cap = nullptr;
        size_t capLen = 0;
        FILE* mem = open_memstream(&cap, &capLen);
        SentencePrinter pr(mem);
        pr.feed("Got it. <track id=\"book-androids\" kind=\"alias\" "
                "text=\"the android");
        std::string first(cap, capLen);
        pr.feed(" book\"/>");
        pr.finish();
        fclose(mem);
        std::string out(cap, capLen);
        free(cap);
        check(first == "Got it. ",
              "holds back a split <track .../> tag");
        check(out == "Got it. ", "removes a split <track .../> tag");
        check(pr.trackRequests().size() == 1,
              "detects one <track .../> request");
        check(pr.trackRequests()[0].id == "book-androids" &&
                  pr.trackRequests()[0].kind == "alias" &&
                  pr.trackRequests()[0].text == "the android book",
              "reads the <track .../> attributes");
    }
    {
        ActionSpec action;
        check(parseAction("<action name=\"read_file\">\n"
                          "<arg name=\"path\">books/one.txt</arg>\n"
                          "<arg name=\"offset\">40</arg>\n"
                          "</action>",
                          action),
              "parses an action block");
        check(action.name == "read_file", "reads the action name");
        check(action.args["path"] == "books/one.txt", "reads an action arg");
        check(action.args["offset"] == "40", "reads a numeric action arg");
        check(!parseAction("no action here", action),
              "rejects text without an action");
        check(parseAction("<action name=\"think\" note=\"done\"/>", action) &&
                  action.name == "think" && action.args["note"] == "done",
              "parses a self-closing action");
    }
    {
        std::string why;
        check(previewVerdict("<approve/>", why) == PreviewVerdict::Approve,
              "preview approves on <approve/>");
        check(previewVerdict("<reject>the path does not exist</reject>", why) ==
                      PreviewVerdict::Reject &&
                  why == "the path does not exist",
              "preview rejects with a reason");
        check(previewVerdict("<reject/>", why) == PreviewVerdict::Reject &&
                  why.empty(),
              "preview rejects even without a reason");
        check(previewVerdict("looks reasonable", why) ==
                  PreviewVerdict::Unclear,
              "a reply without a verdict keeps the plan");
        check(previewVerdict("<reject>no</reject><approve/>", why) ==
                  PreviewVerdict::Approve,
              "approval wins when both markers appear");
    }
    {
        ::mkdir("/tmp/opencode", 0700);
        char dir[] = "/tmp/opencode/facts-XXXXXX";
        char* made = ::mkdtemp(dir);
        check(made != nullptr, "creates a temporary fact-store directory");
        if (made) {
            const std::string root = made;
            const std::string file = root + "/book.pdf";
            const std::string storePath = root + "/facts.txt";

            // A real file so the verifier can stat it.
            {
                std::ofstream out(file);
                out << "hello";
            }

            facts::Store store(storePath);
            store.begin("read the novel", "extractor exit 0 with text");

            action::Result wrote;
            wrote.ok = true;
            wrote.output = "wrote 5 bytes to " + file;
            wrote.facts.push_back(facts::make(
                facts::FactKind::Wrote, "file:" + file, file,
                "wrote 5 bytes", facts::FactStatus::Reported, "stat"));
            store.apply(1, "write_file", {{"path", file}}, wrote);

            const std::string first = store.render();
            check(first.find("VERIFIED FACTS") != std::string::npos,
                  "the store verifies a written file with stat");
            check(first.find("file:" + file) != std::string::npos &&
                      first.find("5 bytes") != std::string::npos,
                  "the verified fact carries the real size");

            action::Result run;
            run.ok = true;
            run.output = "script: x\nexit: 0\n---\nhi";
            run.facts.push_back(facts::make(
                facts::FactKind::Run, "run:" + file, file,
                "exit=0; stdout=2 bytes", facts::FactStatus::Verified,
                "waitpid"));
            store.apply(2, "sandbox-run-script", {{"path", file}}, run);
            check(store.render().find("[run] run:" + file) !=
                          std::string::npos &&
                      store.render().find("waitpid") != std::string::npos,
                  "action-reported run facts are kept and verified");

            action::Result failure;
            failure.ok = false;
            failure.output = "error: no such file";
            store.apply(3, "read_file", {{"path", "/nope"}}, failure);
            check(store.render().find("fail:read_file:/nope") !=
                      std::string::npos,
                  "failures are recorded separately");

            const std::string mirrorDir = root + "/mirror";
            ::mkdir(mirrorDir.c_str(), 0700);
            action::Result mirror;
            mirror.ok = true;
            mirror.output =
                "mirrored directory\n"
                "origin: /mnt/p/novels\n"
                "mirror: " + mirrorDir +
                "\n"
                "files: 1, directories: 1, skipped: 0, bytes: 5";
            mirror.facts.push_back(facts::make(
                facts::FactKind::Mirror, "mirror:" + mirrorDir, mirrorDir,
                "origin=/mnt/p/novels; files: 1, bytes: 5",
                facts::FactStatus::Reported, "stat"));
            mirror.facts.push_back(facts::make(
                facts::FactKind::FsDir, "path:" + mirrorDir, mirrorDir,
                "mirrored", facts::FactStatus::Reported, "stat"));
            store.apply(4, "sandbox-directory-mirror",
                        {{"op", "in"}, {"source", "/mnt/p/novels"}}, mirror);
            check(store.render().find("[mirror] mirror:" + mirrorDir) !=
                          std::string::npos &&
                      store.render().find("verified on disk") !=
                          std::string::npos,
                  "a mirrored directory is verified on disk");
            check(store.render().find("[dir] path:" + mirrorDir) !=
                      std::string::npos,
                  "the mirror destination is a verified directory");

            // Change the file so the old write fact is superseded.
            {
                std::ofstream out(file);
                out << "hello again";
            }
            action::Result wrote2;
            wrote2.ok = true;
            wrote2.output = "wrote 11 bytes to " + file;
            wrote2.facts.push_back(facts::make(
                facts::FactKind::Wrote, "file:" + file, file,
                "wrote 11 bytes", facts::FactStatus::Reported, "stat"));
            store.apply(5, "write_file", {{"path", file}}, wrote2);
            check(store.render().find("SUPERSEDED") != std::string::npos,
                  "a changed fact is kept as superseded");

            action::Result listing;
            listing.ok = true;
            listing.output = "path: " + root +
                             "\nentries: 2\n---\nbook.pdf  (11 bytes)\n"
                             "mirror/\n";
            listing.facts.push_back(facts::make(
                facts::FactKind::Listing, "dir:" + root, root,
                "entries: 2; book.pdf (11 bytes), mirror/",
                facts::FactStatus::Verified, "readdir"));
            store.apply(6, "list_dir", {{"path", root}}, listing);
            check(store.render().find("[listing] dir:" + root) !=
                      std::string::npos,
                  "a directory listing is recorded as a verified fact");

            facts::Store resumed(storePath);
            resumed.begin("read the novel", "ignored");
            check(resumed.revision() == 6 &&
                      resumed.render().find("file:" + file) !=
                          std::string::npos,
                  "the store reloads for the same target");

            facts::Store other(storePath);
            other.begin("a different target", "");
            check(other.render().find("file:" + file) == std::string::npos,
                  "a different target starts a fresh store");

            facts::Store snapshot(storePath);
            snapshot.load();
            check(snapshot.target() == "a different target",
                  "the task store can be read without starting a target");

            {
                facts::Store evictStore(storePath + ".evict");
                evictStore.begin("eviction test", "");
                for (int i = 0; i < 81; ++i) {
                    action::Result many;
                    many.ok = true;
                    many.facts.push_back(facts::make(
                        facts::FactKind::Generic,
                        "generic:" + std::to_string(i), "x",
                        "value " + std::to_string(i),
                        facts::FactStatus::Reported, "test"));
                    evictStore.apply(i, "test", {}, many);
                }
                const auto evicted = evictStore.takeEvicted();
                check(!evicted.empty(),
                      "buffers facts evicted for space");
                check(evictStore.takeEvicted().empty(),
                      "takeEvicted drains the buffer");
                std::remove((storePath + ".evict").c_str());
            }

            std::remove(file.c_str());
            std::remove(storePath.c_str());
            ::rmdir(mirrorDir.c_str());
            ::rmdir(made);
        }
    }
    {
        ::mkdir("/tmp/opencode", 0700);
        char dir[] = "/tmp/opencode/guards-XXXXXX";
        char* made = ::mkdtemp(dir);
        check(made != nullptr, "creates a temporary guards directory");
        if (made) {
            guards::RepeatGuard repeat;
            const std::map<std::string, std::string> args = {{"path", "/tmp/x"}};
            check(repeat.check("read_file", args, 0).allow,
                  "repeat guard allows the first action");
            repeat.observe("read_file", args, 1);
            const guards::Verdict first = repeat.check("read_file", args, 1);
            check(!first.allow && !first.fatal,
                  "repeat guard refuses an identical no-op once");
            const guards::Verdict second = repeat.check("read_file", args, 1);
            check(!second.allow && !second.fatal,
                  "repeat guard refuses again but leaves stopping to the "
                  "motivation signal");
            check(repeat.check("read_file", args, 2).allow,
                  "repeat guard allows the action after a fact changes");

            const std::string storePath = std::string(made) + "/facts.txt";
            facts::Store empty(storePath);
            empty.begin("write a report", "a verified report file exists");
            // A tool was used (a listing) but no output was produced.
            action::Result listing;
            listing.ok = true;
            listing.output = "path: /tmp\nentries: 0\n---\n";
            listing.facts.push_back(facts::make(
                facts::FactKind::Listing, "dir:/tmp", "/tmp", "entries: 0",
                facts::FactStatus::Verified, "readdir"));
            empty.apply(1, "list_dir", {{"path", "/tmp"}}, listing);
            check(!guards::checkDone(empty, {}).allow,
                  "done is refused when the declared product has no output");
            check(guards::checkGiveUp(empty).allow,
                  "give-up is allowed when there is no output");
            check(guards::checkDone(empty, {"dir:/tmp"}).allow,
                  "a named verified listing can be the product");

            action::Result run;
            run.ok = true;
            run.output = "script: x\nexit: 0\n---\nreport";
            run.facts.push_back(facts::make(
                facts::FactKind::Run, "run:x", "x", "exit=0; stdout=6 bytes",
                facts::FactStatus::Verified, "waitpid"));
            empty.apply(2, "sandbox-run-script", {{"path", "x"}}, run);
            check(guards::checkDone(empty, {}).allow,
                  "done is allowed after a verified successful run");
            check(guards::checkDone(empty, {"run:x"}).allow,
                  "a named verified run is accepted as evidence");
            check(!guards::checkDone(empty, {"run:missing"}).allow,
                  "evidence naming an unknown fact is refused");
            check(!guards::checkDone(empty, {"fail:read_file:/nope"}).allow,
                  "evidence naming a missing fact is refused");
            {
                const std::string annotated =
                    "run:x (cycle 29: rc 0, 6 bytes)";
                const auto evidence = guards::doneEvidence(
                    "<done evidence=\"" + annotated + "\"/>");
                check(evidence.size() == 1 && evidence[0] == annotated,
                      "a parenthesised note stays one evidence item");
                const guards::Verdict bad =
                    guards::checkDone(empty, evidence);
                check(!bad.allow &&
                          bad.reason.find("bare fact key") != std::string::npos,
                      "annotated evidence is refused with the bare-key hint");
                const auto two = guards::doneEvidence(
                    "<done evidence=\"run:x, dir:/tmp\"/>");
                check(two.size() == 2 && two[0] == "run:x" &&
                          two[1] == "dir:/tmp",
                      "comma-separated evidence keys are still split");
            }
            check(!guards::checkGiveUp(empty).allow,
                  "give-up is refused while verified output exists");

            const std::vector<std::string> keys =
                guards::doneEvidence("<done evidence=\"run:x, dir:/tmp\"/>");
            check(keys.size() == 2 && keys[0] == "run:x" && keys[1] == "dir:/tmp",
                  "done evidence splits comma-separated keys");
            check(guards::doneEvidence("<done/>").empty(),
                  "a bare done tag has no evidence keys");

            facts::Store open(storePath + ".open");
            open.begin("think about the problem", "");
            check(guards::checkDone(open, {}).allow,
                  "done stays the model's call for an open-ended target");

            facts::Store chat(storePath + ".chat");
            chat.begin("answer the question", "a correct answer");
            check(guards::checkDone(chat, {}).allow,
                  "done stays the model's call when no tool was used");

            std::remove(storePath.c_str());
            std::remove((storePath + ".open").c_str());
            std::remove((storePath + ".chat").c_str());
            ::rmdir(made);
        }
    }
    {
        motivation::State m;
        check(m.value == 50, "motivation starts neutral at 50");
        m.progress(20);
        check(m.value == 70 && m.noProgress == 0 && m.goodStreak == 1,
              "progress raises motivation and clears the dead-end streak");
        m.setback(30);
        check(m.value == 40 && m.noProgress == 1 && m.goodStreak == 0,
              "a setback lowers motivation and counts a dead end");
        m.progress(100);
        check(m.value == 100, "motivation is clamped at 100");
        m.setback(100);
        check(m.value == 0, "motivation is clamped at 0");
        m.decay();
        check(m.value == 1, "decay drifts up from zero toward neutral");
        m.value = 100;
        m.decay();
        check(m.value == 99, "decay drifts down from the maximum");
        motivation::State high;
        motivation::State low;
        high.value = 90;
        low.value = 10;
        int highYes = 0;
        int lowYes = 0;
        for (unsigned seed = 0; seed < 1000; ++seed) {
            if (high.encourages(seed)) ++highYes;
            if (low.encourages(seed)) ++lowYes;
        }
        check(highYes > lowYes * 3,
              "high motivation encourages more often than low motivation");
        check(high.render(1).find("Motivation for this target: 90/100") !=
                  std::string::npos,
              "the motivation block reports its value");
        check(high.line("test").find("motivation:") == 0,
              "the motivation log line starts with motivation");
        motivation::State stall;
        stall.noFact();
        stall.noFact();
        check(stall.render(1).find("Stall signal") == std::string::npos,
              "two no-fact steps do not raise the stall signal");
        stall.noFact();
        check(stall.stallStreak() == 3 &&
                  stall.render(1).find("Stall signal") != std::string::npos,
              "three no-fact steps raise the stall signal");
        stall.progress(20);
        check(stall.stallStreak() == 3,
              "observer progress alone does not erase the no-fact streak");
        stall.factMade();
        check(stall.stallStreak() == 0 &&
                  stall.render(1).find("Stall signal") == std::string::npos,
              "a real fact clears the stall signal");
        motivation::Store store;
        motivation::State& first = store.forTarget("target A");
        first.progress(30);
        check(first.value == 80, "the store rewards the selected target");
        motivation::State& second = store.forTarget("target B");
        check(second.value == 50, "a different target starts at neutral");
        check(store.forTarget("target A").value == 80,
              "each target keeps its own motivation value");
    }
    {
        const char* oldHomeEnv = getenv("AI_CHAT_HOME");
        const std::string oldHome = oldHomeEnv ? oldHomeEnv : "";
        ::mkdir("/tmp/opencode", 0700);
        char dir[] = "/tmp/opencode/experience-XXXXXX";
        char* made = ::mkdtemp(dir);
        check(made != nullptr, "creates a temporary home for experience tests");
        if (made) {
            const std::string home = made;
            const std::string target = "extract the opening of a PDF";
            const std::string other = "name the cat";
            setenv("AI_CHAT_HOME", home.c_str(), 1);
            const std::string id = experience::begin(
                target,
                "Expected product: 30 clean lines\n"
                "Context: a scanned PDF\n");
            check(!id.empty(), "experience begin creates a target directory");
            const std::string root = experience::rootPath();
            const std::string desc = root + "/" + id + "/" + id + ".desc";
            const std::string loadedDesc = experience::load(target);
            check(loadedDesc.find("Expected product: 30 clean lines") !=
                      std::string::npos,
                  "the expected product is stored in .desc");
            check(loadedDesc.find("Context: a scanned PDF") !=
                      std::string::npos,
                  "the .desc keeps the detailed context");

            const std::string expPath =
                experience::record(target, "solution",
                                   "use extract_unicode.py for CJK PDFs");
            check(!expPath.empty(), "a solution is recorded");
            const std::string lessonPath = experience::recordLesson(
                target,
                "PURPOSE: To avoid a bad extract\n"
                "PRACTICE: use relative paths\n"
                "EVIDENCE: 2026-10-26 a relative path worked");
            check(!lessonPath.empty(), "a practice lesson is recorded");

            action::setLessonTarget(target);
            action::Request lessonRequest;
            lessonRequest.name = "lesson";
            lessonRequest.args["text"] =
                "PURPOSE: To keep a rule found mid-run\n"
                "PRACTICE: record it while it is still visible\n"
                "EVIDENCE: this self-test";
            const action::Result lessonResult =
                action::execute(lessonRequest);
            check(lessonResult.ok,
                  "the lesson action records practice through the app");
            check(experience::load(target).find("record it while it is still "
                                                "visible") != std::string::npos,
                  "the lesson action's text reaches the target .exp");
            action::setLessonTarget("");
            const std::string skillPath = experience::record(
                target, "target reached",
                "solution:\nkeep this skill\nevidence:\ntest run\n");
            check(!skillPath.empty(), "a legacy skill block is recorded");
            const std::string loaded = experience::load(target);
            check(loaded.find("extract_unicode.py") != std::string::npos &&
                      loaded.find("use relative paths") != std::string::npos,
                  "practice is loaded for the same target");

            const std::string stepPath = experience::recordStep(
                target, "good step", "plan: read\nobservation: ok");
            check(!stepPath.empty(), "a step is recorded in its own ledger");
            check(experience::load(target).find("observation: ok") ==
                      std::string::npos,
                  "the step ledger is not dumped into the prompt");
            {
                std::ifstream steps(stepPath);
                std::stringstream body;
                body << steps.rdbuf();
                check(body.str().find("observation: ok") != std::string::npos,
                      "the step text reaches .steps");
            }

            const std::string statePath = experience::writeState(
                target, "Frontier: pages 16-17\nCovered: chapter 1\n"
                        "Open: page anchors\nNext: continue");
            check(!statePath.empty(),
                  "a state is written in its own file");
            const std::string journalPath = experience::recordJournal(
                target, "I liked the kipple passage because Isidore keeps "
                        "offering to clean a room he cannot save.");
            check(!journalPath.empty(),
                  "a first-person journal entry is recorded in its own file");
            const std::string loadedRecords = experience::load(target);
            check(loadedRecords.find("Current state (.state)") !=
                          std::string::npos &&
                      loadedRecords.find("Frontier: pages 16-17") !=
                          std::string::npos,
                  "the current state is loaded for the same target");
            check(loadedRecords.find("Reader journal") != std::string::npos &&
                      loadedRecords.find("kipple passage") != std::string::npos,
                  "the reader journal is loaded for the same target");
            check(experience::load(target).find("observation: ok") ==
                      std::string::npos,
                  "the journal and state stay separate from the step ledger");

            const std::string state2 = experience::writeState(
                target, "Frontier: page 18\nCovered: pages 16-17\n"
                        "Open: the rest\nNext: page 18");
            check(!state2.empty(), "a state rewrite succeeds");
            const std::string rewritten = experience::load(target);
            check(rewritten.find("Frontier: page 18") != std::string::npos &&
                      rewritten.find("Frontier: pages 16-17") ==
                          std::string::npos,
                  "the state is replaced, not appended");
            {
                std::ifstream prev(statePath + ".prev");
                std::stringstream body;
                body << prev.rdbuf();
                check(body.str().find("Frontier: pages 16-17") !=
                          std::string::npos,
                      "the previous state is kept for recovery");
            }
            check(experience::loadState(target).find("Frontier: page 18") !=
                      std::string::npos,
                  "loadState returns the current state body");

            const std::string state3 = experience::writeState(
                target, "# state: echoed\n# updated: now\n\n"
                        "Frontier: page 19\nCovered: through 18\n"
                        "Open: rest\nNext: page 19");
            check(!state3.empty() &&
                      experience::loadState(target).find("# state: echoed") ==
                          std::string::npos,
                  "an echoed state header is stripped");

            check(experience::queueJournal(target, "entry",
                                           "queued journal line", true) == 0,
                  "queueJournal writes through when the target is writable");

            // A thread shares state and journal across targets.
            const std::string threadedA = experience::begin(
                "threaded target A", "first phase", "reading-thread");
            const std::string threadedB = experience::begin(
                "threaded target B", "second phase", "reading-thread");
            check(!threadedA.empty() && !threadedB.empty(),
                  "two targets can share a thread");
            const std::string threadedState =
                experience::statePath("threaded target A");
            const std::string threadedJournal =
                experience::journalPath("threaded target A");
            check(threadedState.find("/.threads/reading-thread/") !=
                          std::string::npos &&
                      threadedJournal.find("/.threads/reading-thread/") !=
                          std::string::npos,
                  "thread state and journal live under .threads/");
            const std::string threadedStatePath = experience::writeState(
                "threaded target A", "Frontier: page 20\nCovered: page 19\n"
                                     "Open: rest\nNext: page 20");
            const std::string threadedJournalPath = experience::recordJournal(
                "threaded target A", "I noticed the door speech.");
            check(!threadedStatePath.empty() &&
                      threadedStatePath == threadedState,
                  "state is written to the shared thread");
            check(!threadedJournalPath.empty() &&
                      threadedJournalPath == threadedJournal,
                  "journal is written to the shared thread");
            const std::string threadedLoad = experience::load("threaded target B");
            check(threadedLoad.find("Thread: reading-thread") !=
                          std::string::npos &&
                      threadedLoad.find("Frontier: page 20") !=
                          std::string::npos &&
                      threadedLoad.find("I noticed the door speech") !=
                          std::string::npos,
                  "a second target sees the thread state and journal");
            check(experience::loadState("threaded target B").find(
                      "Frontier: page 20") != std::string::npos,
                  "loadState resolves the shared thread");

            // A legacy block with a solution and evidence is a usable skill
            // record: it stays in .exp and is marked as a how-to reference.

            const std::string otherId = experience::begin(other, "");
            check(!otherId.empty() && otherId != id,
                  "a different target gets a different experience directory");
            const std::string otherLoaded = experience::load(other);
            check(otherLoaded.find("extract_unicode.py") ==
                      std::string::npos,
                  "experience is not mixed between targets");
            check(otherLoaded.find("use relative paths") !=
                      std::string::npos,
                  "the cross-target practice digest exposes the lesson");

            // A failed write is queued and retried after the target becomes
            // writable again. The queue keeps the channel it belongs to.
            ::chmod(stepPath.c_str(), 0400);
            check(experience::queue(target, "queued", "retry body") == 1,
                  "queues a failed step write");
            check(experience::pendingCount() == 1,
                  "persists the pending experience queue");
            check(experience::flushPending() == 1,
                  "keeps a failed retry in the queue");
            ::chmod(stepPath.c_str(), 0600);
            check(experience::flushPending() == 0,
                  "flushes the queue after the write succeeds");
            {
                std::ifstream steps(stepPath);
                std::stringstream body;
                body << steps.rdbuf();
                check(body.str().find("retry body") != std::string::npos,
                      "the retried record reaches the step ledger");
            }
            check(experience::pendingCount() == 0,
                  "clears the queue after a successful retry");

            // Legacy .exp files mixed diaries with practice. Migration moves
            // the diary to .steps and keeps only purpose/practice/evidence.
            std::string report;
            check(experience::migrate(report, true),
                  "the migration dry run succeeds");
            check(report.find("dry run") != std::string::npos &&
                      report.find("legacy block") != std::string::npos,
                  "the dry run reports the blocks it would move");
            check(experience::migrate(report, false),
                  "the migration applies cleanly");
            {
                std::ifstream exp(expPath);
                std::stringstream body;
                body << exp.rdbuf();
                const std::string text = body.str();
                check(text.find("PURPOSE:") != std::string::npos &&
                          text.find("extract_unicode.py") ==
                              std::string::npos,
                      "migration keeps practice and removes the old diary");
                check(text.find("compatible legacy - worthy reference for "
                                "how-to") != std::string::npos &&
                          text.find("keep this skill") != std::string::npos,
                      "migration marks a kept legacy skill as a how-to "
                      "reference");
                check(text.find("| target reached ==") != std::string::npos,
                      "the kept legacy block keeps its original header");
                size_t marks = 0;
                for (size_t at = text.find("compatible legacy");
                     at != std::string::npos;
                     at = text.find("compatible legacy", at + 1))
                    ++marks;
                check(marks == 1, "the legacy marker is written once");
            }
            {
                std::ifstream steps(stepPath);
                std::stringstream body;
                body << steps.rdbuf();
                check(body.str().find("extract_unicode.py") !=
                          std::string::npos,
                      "migration moves the old diary into .steps");
            }
            check(experience::migrate(report, false) &&
                      report.find("nothing to migrate") != std::string::npos,
                  "migration is idempotent");

            std::remove(experience::pendingPath().c_str());
            std::remove((experience::pendingPath() + ".tmp").c_str());
            for (const auto& entry :
                 std::filesystem::directory_iterator(root)) {
                const std::string name =
                    entry.path().filename().string();
                if (name.rfind(".migration-backup-", 0) == 0)
                    std::filesystem::remove_all(entry.path());
            }
            std::remove(desc.c_str());
            std::remove(expPath.c_str());
            std::remove(stepPath.c_str());
            std::remove(experience::practicePath().c_str());
            ::rmdir((root + "/" + id).c_str());
            std::remove(
                (root + "/" + otherId + "/" + otherId + ".desc").c_str());
            ::rmdir((root + "/" + otherId).c_str());
            ::rmdir(root.c_str());
            ::rmdir((home + "/.config/ai-chat").c_str());
            ::rmdir((home + "/.config").c_str());
            ::rmdir(home.c_str());
            setenv("AI_CHAT_HOME", oldHome.c_str(), 1);
        }
    }
    {
        const char* oldHomeEnv = getenv("AI_CHAT_HOME");
        const std::string oldHome = oldHomeEnv ? oldHomeEnv : "";
        ::mkdir("/tmp/opencode", 0700);
        char dir[] = "/tmp/opencode/story-XXXXXX";
        char* made = ::mkdtemp(dir);
        check(made != nullptr, "creates a temporary home for story tests");
        if (made) {
            const std::string home = made;
            setenv("AI_CHAT_HOME", home.c_str(), 1);
            check(story::writeLife("WHO: Rick\nWANTED: a real animal\n"
                                   "OPEN: whether the goat lives"),
                  "the life story is written");
            check(story::loadLife().find("whether the goat lives") !=
                      std::string::npos,
                  "the life story is read back");
            check(story::writeObject("book-androids",
                                     "WHO: Roy\nWANTED: more life\n"
                                     "TURN: the spinneret goes dark"),
                  "an object story is written");
            check(story::loadObject("book-androids").find("spinneret") !=
                      std::string::npos,
                  "the object story is read back");
            action::Request storyRequest;
            storyRequest.name = "story";
            storyRequest.args["text"] = "WHO: Roy\nTURN: the story action";
            storyRequest.args["object"] = "roy";
            check(action::execute(storyRequest).ok,
                  "the story action writes an object story");
            check(story::loadObject("roy").find("the story action") !=
                      std::string::npos,
                  "the story action's text reaches story memory");
            {
                const std::vector<std::string> objects = story::listObjects();
                check(std::find(objects.begin(), objects.end(),
                                "book-androids") != objects.end() &&
                          std::find(objects.begin(), objects.end(), "roy") !=
                              objects.end(),
                      "the object stories are listed");
            }
            const std::string rendered = story::renderForPrompt(4000);
            check(rendered.find("Life story") != std::string::npos &&
                      rendered.find("Object story: book-androids") !=
                          std::string::npos,
                  "the story prompt contains the trunk and its branch");
            check(story::writeObject("book-androids",
                                     "WHO: Roy\nTURN: rewritten"),
                  "an object story can be replaced");
            std::ifstream prev(story::objectPath("book-androids") + ".prev");
            std::stringstream prevBody;
            prevBody << prev.rdbuf();
            check(prevBody.str().find("spinneret") != std::string::npos,
                  "the previous object story is kept");

            check(story::writeObject("fading", "WHO: a quiet object\n"
                                               "OPEN: none"),
                  "a closed object story is written");
            const std::string index = configSubpath("story-index.mem");
            {
                std::ifstream in(index);
                std::stringstream body;
                body << in.rdbuf();
                std::istringstream lines(body.str());
                std::ostringstream rewritten;
                std::string line;
                while (std::getline(lines, line)) {
                    if (line.rfind("fading\t", 0) == 0) {
                        std::istringstream row(line);
                        std::string id, score, last, open, active;
                        std::getline(row, id, '\t');
                        std::getline(row, score, '\t');
                        std::getline(row, last, '\t');
                        std::getline(row, open, '\t');
                        std::getline(row, active, '\t');
                        rewritten << id << '\t' << score << "\t0\t" << open
                                  << '\t' << active << '\n';
                    } else {
                        rewritten << line << '\n';
                    }
                }
                std::ofstream out(index, std::ios::trunc);
                out << rewritten.str();
            }
            story::sweep();
            check(story::loadObject("fading").empty() &&
                      story::renderForPrompt(4000).find(
                          "Dormant object stories") != std::string::npos,
                  "a quiet closed object story fades into the dormant index");
            check(story::writeObject("fading", "WHO: back again\n"
                                               "OPEN: none"),
                  "a dormant object story can be reopened");
            check(story::loadObject("fading").find("back again") !=
                      std::string::npos,
                  "the reopened object story is active again");
            std::filesystem::remove_all(home);
            setenv("AI_CHAT_HOME", oldHome.c_str(), 1);
        }
    }
    {
        const char* oldHomeEnv = getenv("AI_CHAT_HOME");
        const std::string oldHome = oldHomeEnv ? oldHomeEnv : "";
        ::mkdir("/tmp/opencode", 0700);
        char dir[] = "/tmp/opencode/documentary-XXXXXX";
        char* made = ::mkdtemp(dir);
        check(made != nullptr,
              "creates a temporary home for documentary memory tests");
        if (made) {
            const std::string home = made;
            setenv("AI_CHAT_HOME", home.c_str(), 1);
            check(documentary::path().size() > 4 &&
                      documentary::path().compare(
                          documentary::path().size() - 4, 4, ".mem") == 0,
                  "documentary memory uses the .mem extension");

            documentary::Memory memory;
            documentary::Item first{"2026-01-01 10:00",
                                    "2026-01-01 11:00", "A did a thing."};
            documentary::addRaw(memory, first);
            check(memory.hasLive && memory.raw.empty(),
                  "the first raw item stays live");
            check(documentary::render(memory).find(
                      "live raw item, Boundary of Forgetting") !=
                      std::string::npos,
                  "a lone raw item is the Boundary of Forgetting");
            check(!documentary::needsOrganize(memory),
                  "one live item needs no organization");

            documentary::Item second{"2026-01-02 10:00",
                                     "2026-01-02 11:00",
                                     "B did another thing."};
            documentary::addRaw(memory, second);
            check(memory.hasLive && memory.raw.empty() &&
                      memory.live.text.find("A did a thing.") !=
                          std::string::npos &&
                      memory.live.text.find("B did another thing.") !=
                          std::string::npos &&
                      memory.live.end == "2026-01-02 11:00",
                  "a segment that fits is appended to the live item");
            const std::string two = documentary::render(memory);
            check(two.find("A did a thing.") != std::string::npos &&
                      two.find("B did another thing.") != std::string::npos,
                  "the render keeps the accumulated live item");
            check(two.find("A did a thing.") <
                      two.find("B did another thing."),
                  "the accumulated live item keeps event order");
            check(!documentary::needsOrganize(memory),
                  "a partly filled live item needs no organization");

            documentary::Item third{"2026-01-03 10:00",
                                    "2026-01-03 11:00",
                                    "C did a third thing."};
            documentary::addRaw(memory, third);
            check(memory.hasLive && memory.raw.empty() &&
                      !documentary::needsOrganize(memory),
                  "the live item keeps accumulating below the size limit");

            documentary::Memory mixed;
            const std::string twoThousand(2000, 'y');
            const std::string fifteenHundred(1500, 'z');
            documentary::addRaw(
                mixed, {"2026-03-01 10:00", "2026-03-01 11:00",
                        twoThousand});
            documentary::addRaw(
                mixed, {"2026-03-02 10:00", "2026-03-02 11:00",
                        fifteenHundred});
            check(mixed.raw.size() == 1 && mixed.hasLive &&
                      mixed.live.text.size() == 1500,
                  "a segment that does not fit seals the live item");
            documentary::addRaw(
                mixed, {"2026-03-03 10:00", "2026-03-03 11:00",
                        twoThousand});
            check(mixed.raw.size() == 2 && mixed.hasLive &&
                      mixed.live.text.size() == 2000,
                  "the next live item starts with the overflowing segment");

            documentary::Memory full;
            const std::string big(documentary::ITEM_MAX_CHARS, 'x');
            for (int i = 0; i < 8; ++i) {
                documentary::Item item{
                    "2026-02-0" + std::to_string(i + 1) + " 10:00",
                    "2026-02-0" + std::to_string(i + 1) + " 11:00", big};
                documentary::addRaw(full, item);
            }
            check(!full.hasLive && full.raw.size() == 8,
                  "a full live item is sealed and eight raw items accumulate");
            check(documentary::needsOrganize(full),
                  "eight raw items trigger a layer-0 batch");

            documentary::Memory layered;
            layered.hasLive = true;
            layered.live = {"2026-01-05 10:00", "2026-01-05 11:00",
                            "live-content"};
            layered.raw = {{"2026-01-04 10:00", "2026-01-04 11:00",
                            "raw-content"}};
            layered.layer0 = {{"2026-01-03 10:00", "2026-01-03 11:00",
                               "l0-content"}};
            layered.layer1 = {{"2026-01-02 10:00", "2026-01-02 11:00",
                               "l1-content"}};
            layered.last = {{"2026-01-01 10:00", "2026-01-03 11:00",
                             "last-content"}};
            const std::string layers = documentary::render(layered);
            check(layers.find("live-content") < layers.find("raw-content") &&
                      layers.find("raw-content") < layers.find("l0-content") &&
                      layers.find("l0-content") < layers.find("l1-content") &&
                      layers.find("l1-content") < layers.find("last-content"),
                  "the render orders live, raw, layer 0, layer 1 and last");
            check(layers.find("Boundary of Forgetting") !=
                      std::string::npos,
                  "the oldest item is labelled Boundary of Forgetting");
            check(layers.find("Nothing before 2026-01-01 10:00") !=
                      std::string::npos,
                  "the oldest item defines the forgetting boundary");

            documentary::Memory lastPair;
            lastPair.last = {
                {"2026-01-01 10:00", "2026-01-03 11:00", "old boundary"},
                {"2026-01-04 10:00", "2026-01-05 11:00", "newer boundary"},
            };
            check(documentary::needsOrganize(lastPair),
                  "two last items trigger a boundary merge");

            check(documentary::save(mixed), "saves documentary memory");
            documentary::Memory loaded = documentary::load();
            check(loaded.hasLive &&
                      loaded.live.text.size() == 2000 &&
                      loaded.live.start == "2026-03-03 10:00",
                  "loads the live raw item");
            check(loaded.raw.size() == 2 &&
                      loaded.raw[0].start == "2026-03-01 10:00" &&
                      loaded.raw[1].start == "2026-03-02 10:00",
                  "loads sealed raw items oldest first");
            check(documentary::render(loaded).find(
                      "Nothing before 2026-03-01 10:00") !=
                      std::string::npos,
                  "the oldest item defines the forgetting boundary");
            check(documentary::ITEM_MAX_WORDS == 500 &&
                      documentary::ITEM_MAX_CHARS == 3000,
                  "all documentary layers share one size limit");

            const std::string capped = documentary::render(loaded, 800);
            check(capped.size() < 1200 &&
                      capped.find("Boundary of Forgetting") !=
                          std::string::npos,
                  "the prompt-sized documentary view keeps the Boundary");

            std::remove(documentary::path().c_str());
            std::remove((documentary::path() + ".tmp").c_str());
            ::rmdir((home + "/.config/ai-chat").c_str());
            ::rmdir((home + "/.config").c_str());
            ::rmdir(home.c_str());
            setenv("AI_CHAT_HOME", oldHome.c_str(), 1);
        }
    }
    {
        const char* oldHomeEnv = getenv("AI_CHAT_HOME");
        const std::string oldHome = oldHomeEnv ? oldHomeEnv : "";
        ::mkdir("/tmp/opencode", 0700);
        char dir[] = "/tmp/opencode/identity-XXXXXX";
        char* made = ::mkdtemp(dir);
        check(made != nullptr,
              "creates a temporary home for identity tracker tests");
        if (made) {
            const std::string home = made;
            setenv("AI_CHAT_HOME", home.c_str(), 1);

            identity::Tracker tracker = identity::load();
            check(tracker.objects.empty(),
                  "the identity tracker starts empty");
            check(identity::path().find("identity-tracker.mem") !=
                      std::string::npos,
                  "the identity tracker uses identity-tracker.mem");

            std::string id;
            std::string note;
            check(identity::track(
                      tracker, "[Human] Long: my user; creator; who loves me",
                      id, note),
                  "tracks a human object");
            check(id == "human-long", "uses a readable object id");
            const identity::Object* longObject =
                identity::find(tracker, "Long");
            check(longObject && longObject->lines.size() == 3,
                  "stores all three human identities");
            check(longObject && !longObject->lines.empty() &&
                      longObject->lines[0].kind == "role",
                  "Human identities default to roles");

            check(identity::track(tracker, "[Human] Long: role: my user", id,
                                  note),
                  "updates an existing object");
            longObject = identity::find(tracker, "Long");
            check(longObject && longObject->lines.size() == 3,
                  "dedupes an identical identity");

            check(identity::track(tracker, "[Human] Someone: father", id, note),
                  "tracks a person with a father role");
            check(identity::track(tracker, "[Human] Someone: role: dad", id,
                                  note),
                  "tracks the same role under a synonym");
            const identity::Object* someone =
                identity::find(tracker, "Someone");
            check(someone && someone->lines.size() == 1,
                  "dedupes father and dad");

            check(identity::track(
                      tracker,
                      "[Book] Do Androids Dream of Electric Sheep?: fact: "
                      "Chinese edition, Yilin 2013; relation: book Long asked "
                      "me to read",
                      id, note),
                  "tracks a book object");

            check(identity::track(tracker, "[AI] Chonky Orca: myself", id,
                                  note),
                  "tracks the AI object");
            check(identity::track(
                      tracker,
                      "[AI] CO: someone Long mentioned; really naughty", id,
                      note),
                  "tracks an alias object");
            check(identity::track(
                      tracker,
                      "[AI] ?7f3a: someone Long said he created; really naughty",
                      id, note),
                  "tracks an uncertain object");

            std::string mergeNote;
            check(identity::merge(tracker, "CO", "Chonky Orca",
                                  "Long said CO is you", mergeNote),
                  "merges an alias object into the AI object");
            const identity::Object* sourceTomb =
                identity::find(tracker, "ai-co");
            check(sourceTomb &&
                      sourceTomb->status.rfind("merged-into:", 0) == 0,
                  "the merged source is tombstoned");
            const identity::Object* ai =
                identity::find(tracker, "Chonky Orca");
            check(ai && ai->lines.size() >= 4 && !ai->mergedFrom.empty(),
                  "the target carries the source identities and merge record");
            check(identity::find(tracker, "CO") == ai,
                  "the merged object answers to the source name");

            check(identity::merge(tracker, "?7f3a", "Chonky Orca", "",
                                  mergeNote),
                  "merges the uncertain object");
            const std::string rendered = identity::render(tracker);
            check(rendered.find("[AI] Chonky Orca") != std::string::npos &&
                      rendered.find("really naughty") != std::string::npos,
                  "the render shows the merged object and its identities");
            check(rendered.find("?7f3a") == std::string::npos,
                  "the render hides merged objects");

            const identity::Object* userObject =
                identity::primaryWithRole(tracker, "user");
            check(userObject && userObject->id == "human-long",
                  "finds the user object by role");

            check(normalizeLoginId("  alice  ") == "alice" &&
                      normalizeLoginId("alice/bob").empty() &&
                      normalizeLoginId("a lice").empty(),
                  "cleans and validates login-ids");

            std::string loginNote;
            check(identity::bindLogin(tracker, "alice", loginNote) &&
                      loginNote.find("created") != std::string::npos,
                  "creates a tracked person for a new login-id");
            const identity::Object* alice =
                identity::objectForLogin(tracker, "ALICE");
            check(alice && alice->category == "Human" &&
                      alice->source == "login" &&
                      alice->name == "alice",
                  "the login-id resolves to its tracked person");
            check(identity::bindLogin(tracker, "alice", loginNote) &&
                      loginNote.empty(),
                  "an existing login binding is not duplicated");
            check(identity::bindLogin(tracker, "Long", loginNote) &&
                      identity::objectForLogin(tracker, "long") ==
                          identity::find(tracker, "Long"),
                  "an existing person with the same name is bound instead");
            check(!identity::addLine(tracker, alice->id, "login", "eve",
                                     loginNote),
                  "the model cannot rebind a login-id through <track>");

            const identity::Object* book =
                identity::find(tracker, "Do Androids Dream of Electric Sheep?");
            check(book != nullptr, "finds the book object");
            const std::string bookId = book ? book->id : std::string();
            std::string lineNote;
            check(identity::addLine(tracker, bookId, "alias",
                                    "the android book", lineNote),
                  "adds a confirmed alias with <track>");
            check(!identity::addLine(tracker, bookId, "alias",
                                     "the android book", lineNote),
                  "rejects a duplicate alias");
            check(!identity::addLine(tracker, "missing-object", "alias",
                                     "nowhere", lineNote),
                  "rejects an unknown object id");

            identity::ViewContext viewCtx;
            viewCtx.focus.push_back("Long");
            const identity::View view =
                identity::renderView(tracker, viewCtx, 6000, 2000);
            check(view.text.find("[Human] Long") != std::string::npos,
                  "the focus object is in the details section");
            check(view.text.find("Identity tracker — index") !=
                      std::string::npos,
                  "the remaining objects are in the index");

            identity::ViewContext aliasCtx;
            aliasCtx.user_message = "what about that android book?";
            const identity::View aliasView =
                identity::renderView(tracker, aliasCtx, 6000, 2000);
            check(aliasView.text.find("[Book] Do Androids") !=
                      std::string::npos &&
                      aliasView.text.find("the android book") !=
                          std::string::npos,
                  "the confirmed alias pulls the book into details");
            check(aliasView.text.find("Identity tracker — details") <
                      aliasView.text.find("[Book] Do Androids"),
                  "the alias match puts the book before the index");

            const std::vector<std::string> derived =
                identity::focusFromText(tracker,
                                        "please open the android book", 3);
            check(!derived.empty() &&
                      derived[0] ==
                          "Do Androids Dream of Electric Sheep?",
                  "derives a short focus from a text mention");

            identity::ViewContext noFocus;
            const identity::View tight =
                identity::renderView(tracker, noFocus, 200, 100);
            check(tight.text.size() < 1000 && tight.omitted > 0,
                  "the view respects the detail and index budgets");

            check(identity::save(tracker), "saves the identity tracker");
            identity::Tracker loaded = identity::load();
            const identity::Object* reloaded =
                identity::find(loaded, "Chonky Orca");
            check(reloaded && reloaded->lines.size() >= 4 &&
                      reloaded->mergedFrom.size() >= 2,
                  "loads the merged object and its history");
            check(identity::objectForLogin(loaded, "alice") &&
                      identity::objectForLogin(loaded, "long") ==
                          identity::find(loaded, "Long"),
                  "loads login bindings from identity-tracker.mem");

            std::string forgetNote;
            check(identity::forget(loaded, "Long", forgetNote),
                  "retires an object");
            check(identity::render(loaded).find("[Human] Long") ==
                      std::string::npos,
                  "a retired object is hidden from the render");
            const identity::Object* retired = identity::find(loaded, "Long");
            check(retired && retired->status == "retired",
                  "a retired object stays on disk");

            // Tombstone archiving: keep two, move the rest to history.
            for (int i = 0; i < 4; ++i) {
                const std::string spec =
                    "[Tool] oldtool" + std::to_string(i) + ": fact: old";
                identity::track(loaded, spec, id, note);
                identity::forget(loaded, "oldtool" + std::to_string(i), note);
            }
            std::string archiveNote;
            check(identity::compact(loaded, 2, archiveNote),
                  "archives old tombstones");
            check(archiveNote.find("identity-history.mem") !=
                      std::string::npos,
                  "reports the tombstone archive");
            size_t tombstones = 0;
            for (const identity::Object& o : loaded.objects)
                if (o.status != "active") ++tombstones;
            check(tombstones == 2, "keeps only the newest tombstones");
            {
                std::ifstream history(identity::historyPath());
                std::ostringstream text;
                text << history.rdbuf();
                check(text.str().find("oldtool") != std::string::npos,
                      "writes archived tombstones to identity-history.mem");
            }
            check(identity::save(loaded), "saves the compacted tracker");

            // One-time migration of an old identity fact from the task store.
            std::remove(identity::path().c_str());
            const std::string taskPath = configSubpath("autonomy-facts.mem");
            {
                std::ofstream out(taskPath);
                out << "# ai-chat fact store v1\n"
                    << "target\ttest\n"
                    << "done_when\t\n"
                    << "revision\t1\n"
                    << "last_cycle\t0\n"
                    << "fact\tidentity:1\tuser\tthe legacy fact\t0\t1\t"
                       "remembered\tremembered\t1\t1\n"
                    << "fact\ttask:1\tx\ttask fact\t0\t1\t\t\t1\t0\n";
            }
            identity::Tracker migrated = identity::load();
            const identity::Object* legacy =
                identity::find(migrated, "?legacy");
            check(legacy &&
                      identity::render(migrated).find("the legacy fact") !=
                          std::string::npos,
                  "migrates an old identity fact into the tracker");
            {
                std::ifstream in(taskPath);
                std::ostringstream text;
                text << in.rdbuf();
                const std::string taskText = text.str();
                check(taskText.find("task fact") != std::string::npos &&
                          taskText.find("the legacy fact") ==
                              std::string::npos,
                      "the migrated fact is removed from the task store");
            }

            std::remove(identity::path().c_str());
            std::remove((identity::path() + ".tmp").c_str());
            std::remove(identity::historyPath().c_str());
            std::remove((identity::historyPath() + ".tmp").c_str());
            std::remove(taskPath.c_str());
            std::remove((taskPath + ".tmp").c_str());
            ::rmdir((home + "/.config/ai-chat").c_str());
            ::rmdir((home + "/.config").c_str());
            ::rmdir(home.c_str());
            setenv("AI_CHAT_HOME", oldHome.c_str(), 1);
        }
    }
    {
        action::Result yes;
        yes.ok = true;
        yes.output = "did a thing";
        const auto generic = facts::fallback("mystery_action", {}, yes, 7);
        check(generic.size() == 1 &&
                  generic[0].kind == facts::FactKind::Generic &&
                  generic[0].cycle == 7 &&
                  generic[0].source == "mystery_action",
              "the fallback gives an unknown action a generic fact");

        action::Result no;
        no.ok = false;
        no.output = "error: nope";
        const auto failed =
            facts::fallback("read_file", {{"path", "/x"}}, no, 8);
        check(failed.size() == 1 &&
                  failed[0].kind == facts::FactKind::Failure &&
                  failed[0].key == "fail:read_file:/x",
              "the fallback records a failure");

        action::Result thought;
        thought.ok = true;
        thought.output = "thought";
        check(facts::fallback("think", {}, thought, 9).empty(),
              "think adds no fact");
    }
    {
        // The write/read/list actions report their own typed facts. Exercise
        // them through action::execute so the resolved paths are real.
        const std::string ws = action::workspacePath();
        ::mkdir(ws.c_str(), 0700);
        const std::string dir =
            ws + "/facts-action-" + std::to_string(::getpid());
        ::mkdir(dir.c_str(), 0700);
        const std::string file = dir + "/report.txt";

        action::Request wr;
        wr.name = "write_file";
        wr.args["path"] = file;
        wr.args["content"] = "hello";
        const action::Result wrote = action::execute(wr);
        check(wrote.ok && !wrote.facts.empty(),
              "write_file reports a typed fact");
        check(!wrote.facts.empty() &&
                  wrote.facts[0].kind == facts::FactKind::Wrote &&
                  wrote.facts[0].subject == file,
              "the write fact carries the resolved path");

        action::Request rd;
        rd.name = "read_file";
        rd.args["path"] = file;
        const action::Result read = action::execute(rd);
        check(read.ok && !read.facts.empty() &&
                  read.facts[0].kind == facts::FactKind::FsFile &&
                  read.facts[0].value.find("5 bytes") != std::string::npos,
              "read_file reports a verified file fact with the real size");

        action::Request ls;
        ls.name = "list_dir";
        ls.args["path"] = dir;
        const action::Result listed = action::execute(ls);
        check(listed.ok && !listed.facts.empty() &&
                  listed.facts[0].kind == facts::FactKind::Listing &&
                  listed.facts[0].value.find("report.txt") !=
                      std::string::npos &&
                  listed.facts[0].value.find("5 bytes") != std::string::npos,
              "list_dir reports a structured listing");

        action::Request ed;
        ed.name = "edit_file";
        ed.args["path"] = file;
        ed.args["old"] = "hello";
        ed.args["new"] = "hello\n=== page 8 ===";
        const action::Result edited = action::execute(ed);
        check(edited.ok && !edited.facts.empty() &&
                  edited.facts[0].kind == facts::FactKind::Wrote &&
                  edited.facts[0].value.find("edited at byte 0") !=
                      std::string::npos,
              "edit_file reports a verified content fact at the replaced "
              "offset");

        action::Request gr;
        gr.name = "grep_file";
        gr.args["path"] = file;
        gr.args["pattern"] = "=== page";
        const action::Result grepped = action::execute(gr);
        check(grepped.ok &&
                  grepped.output.find("2:6:=== page 8 ===") !=
                      std::string::npos,
              "grep_file prints line, byte offset and the matching line");

        action::Request amb;
        amb.name = "edit_file";
        amb.args["path"] = file;
        amb.args["old"] = "l";
        amb.args["new"] = "L";
        const action::Result ambiguous = action::execute(amb);
        check(!ambiguous.ok &&
                  ambiguous.output.find("more than once") !=
                      std::string::npos,
              "edit_file refuses an ambiguous replacement instead of "
              "guessing");

        const std::string storePath = "/tmp/opencode/facts-action-store.txt";
        facts::Store store(storePath);
        store.begin("test the action facts", "");
        store.apply(1, "list_dir", ls.args, listed);
        check(store.render().find("entries: 1") != std::string::npos,
              "the structured listing reaches the fact store");

        std::remove(file.c_str());
        ::rmdir(dir.c_str());
        std::remove(storePath.c_str());
    }
    {
        Energy e(1000);
        double left = 0.0;
        check(e.spend(5.0, left) && std::fabs(left - 995.0) < 1e-9,
              "spends the first step cost");
        check(!e.spend(1001.0, left) && std::fabs(left - 995.0) < 1e-9,
              "refuses to spend more energy than is available");
        check(e.spend(1.25, left) && std::fabs(left - 993.75) < 1e-9,
              "supports fractional energy costs");
        e.refill();
        check(std::fabs(e.peek() - 1000.0) < 1e-9,
              "an energy potion refills the budget");
        check(!e.charge(1001.0, left) && std::fabs(left) < 1e-9,
              "token charges clamp the budget at zero instead of going negative");
    }
    {
        check(std::fabs(bankersRound2(2.345) - 2.34) < 1e-9,
              "rounds a decimal half down to the even neighbour");
        check(std::fabs(bankersRound2(2.355) - 2.36) < 1e-9,
              "rounds a decimal half up to the even neighbour");
        check(amountText(1000.0) == "1000.00",
              "formats a whole energy value with two decimals");
        check(amountText(1.1) == "1.10",
              "formats a fractional value with two decimals");
        check(std::fabs(energyCostForTokens(5000, 1) - 1.10) < 1e-9,
              "5000 tokens in cycle 1 cost 1.1 energy");
        check(std::fabs(energyCostForTokens(10000, 2) - 2.44) < 1e-9,
              "applies the cycle factor to the token cost");
        check(energyCostForTokens(0, 5) == 0.0,
              "no tokens cost no energy");
    }
    {
        std::string got;
        check(extractDelta("{\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}",
                           got) && got == "Hi",
              "extracts a normal delta");
        check(extractDelta("{\"choices\":[{\"delta\":{\"content\":null}}]}",
                           got) == false,
              "ignores null content");
        check(extractDelta("{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}",
                           got) == false,
              "ignores the role-only first chunk");
        long long promptTokens = 0;
        long long completionTokens = 0;
        long long totalTokens = 0;
        check(extractUsage(
                  "{\"choices\":[],\"usage\":{\"prompt_tokens\":120,"
                  "\"completion_tokens\":30,\"total_tokens\":150}}",
                  promptTokens, completionTokens, totalTokens) &&
                  promptTokens == 120 && completionTokens == 30 &&
                  totalTokens == 150,
              "extracts token usage from a stream chunk");
        check(!extractUsage("{\"choices\":[],\"usage\":null}", promptTokens,
                            completionTokens, totalTokens),
              "ignores a null usage object");
        check(extractDelta(
                  "{\"choices\":[{\"delta\":{\"content\":\"a\\\\nb\\\"c\\u4f60\"}}]}",
                  got) &&
                  got == "a\\nb\"c\xe4\xbd\xa0",
              "unescapes inside a delta");
    }

    check(userRequestsUntimedWait("Wait for me, no timer please"),
          "detects an explicit wait request");
    check(userRequestsUntimedWait("give me a minute"),
          "detects a request for more time");
    check(userRequestsUntimedWait("WAIT UNTIL I ANSWER"),
          "matches case-insensitively");
    check(userRequestsUntimedWait("this will take a while"),
          "detects a long-time request without an exact duration");
    check(userRequestsUntimedWait("I need a long time for this"),
          "detects an explicit long-time request");
    check(!userRequestsUntimedWait("what's a good name for my cat?"),
          "no wait request in an ordinary message");
    check(!userRequestsUntimedWait("/timeout 0"),
          "commands are not wait requests");

    check(userRequestsExactTime("wait 5 minutes"),
          "detects an exact duration");
    check(userRequestsExactTime("I'll be back in 10 minutes"),
          "detects an exact time to come back");
    check(userRequestsExactTime("wait until 15:30"),
          "detects a clock time");
    check(userRequestsExactTime("give me five minutes"),
          "detects a duration written as words");
    check(userRequestsExactTime("wait till 5"),
          "detects a bare clock hour after a waiting cue");
    check(!userRequestsExactTime("wait for me, no timer"),
          "no exact time in an untimed wait request");
    check(!userRequestsExactTime("wait until I'm done"),
          "no exact time in a vague wait request");

    check(userRequestsContinue("continue please"),
          "detects a request to continue");
    check(userRequestsContinue("go on, keep working"),
          "detects a longer continue request");
    check(!userRequestsContinue("don't continue, stop"),
          "a negative continue request is rejected");
    check(!userRequestsContinue("what is the weather?"),
          "ordinary chat is not a continue request");

    check(action::catalog().find("local_dir_access") != std::string::npos,
          "local-dir-access is in the action catalog");
    check(action::catalog().find("lesson(text)") != std::string::npos,
          "the lesson action is in the action catalog");
    check(action::catalog().find("story(text") != std::string::npos,
          "the story action is in the action catalog");
    check(action::catalog().find(
              "self-made-tool(op, name, explanation?, ...)") !=
              std::string::npos,
          "self-made-tools is in the action catalog with its full op set");
    check(action::catalog().find("op=write") != std::string::npos &&
              action::catalog().find("op=import") != std::string::npos &&
              action::catalog().find("op=run") != std::string::npos,
          "the catalog tells the AI to write, import and run inside the tool");
    check(action::catalog().find("sandbox-run-script(path") != std::string::npos,
          "the sandboxed runner is in the action catalog");
    check(action::catalog().find("AI_CHAT_WORKSPACE") != std::string::npos,
          "the runner catalog explains the sandbox workspace variable");
    check(action::catalog().find("sandbox-file-mirror(") != std::string::npos,
          "the file mirror is in the action catalog");
    check(action::catalog().find("sandbox-directory-mirror(") !=
              std::string::npos,
          "the directory mirror is in the action catalog");
    check(action::catalog().find("sandbox-file-mirror(op, source") !=
              std::string::npos,
          "the file mirror catalog names the source argument");
    check(action::catalog().find("sandbox-directory-mirror(op, source") !=
              std::string::npos,
          "the directory mirror catalog names the source argument");

    check(action::readablePathProblem("/ai-chat-probe-does-not-exist")
                  .find("no such file or directory") != std::string::npos,
          "a missing read path is reported as missing");
    check(action::readablePathProblem("/").find(
              "outside the readable roots") != std::string::npos,
          "an existing path outside the roots is reported as unreadable");

    {
        // Self-made tools are readable without a grant, and the grant
        // manifest restores write/run access for the next startup.
        const char* oldHomeEnv = getenv("AI_CHAT_HOME");
        const std::string oldHome = oldHomeEnv ? oldHomeEnv : "";
        ::mkdir("/tmp/opencode", 0700);
        char dir[] = "/tmp/opencode/self-made-XXXXXX";
        char* made = ::mkdtemp(dir);
        check(made != nullptr, "creates a temporary home for tool tests");
        if (made) {
            const std::string home = made;
            const std::string cfg = home + "/.config/ai-chat";
            const std::string tools = cfg + "/self-made-tools";
            const std::string tool = tools + "/probe";
            const std::string script = tool + "/run.py";
            const std::string manifest = tools + "/.grants";
            ::mkdir((home + "/.config").c_str(), 0700);
            ::mkdir(cfg.c_str(), 0700);
            ::mkdir(tools.c_str(), 0700);
            ::mkdir(tool.c_str(), 0700);
            {
                std::ofstream out(script);
                out << "print(1)\n";
            }
            {
                std::ofstream out(manifest);
                out << "probe\n";
            }

            setenv("AI_CHAT_HOME", home.c_str(), 1);
            check(action::readablePathProblem(script).empty(),
                  "a self-made tool file is readable without a grant");
            check(!action::readablePathProblem(manifest).empty(),
                  "the grant manifest stays hidden from the AI");
            self_made_tools::loadGrants();
            check(!self_made_tools::grantedRoot(tool).empty(),
                  "the grant manifest restores tool access at startup");

            auto toolCall = [&](std::map<std::string, std::string> args) {
                action::Request req;
                req.name = "self-made-tool";
                req.args = std::move(args);
                return action::execute(req);
            };

            const action::Result wrote = toolCall(
                {{"op", "write"}, {"name", "probe"}, {"path", "notes.txt"},
                 {"content", "alpha\nbeta\n"}});
            check(wrote.ok &&
                      ::access((tool + "/notes.txt").c_str(), R_OK) == 0,
                  "self-made-tool writes a file into the tool folder");

            const action::Result read = toolCall(
                {{"op", "read"}, {"name", "probe"}, {"path", "notes.txt"}});
            check(read.ok && read.output.find("alpha") != std::string::npos,
                  "self-made-tool reads a file back from the tool folder");

            const action::Result grepped = toolCall(
                {{"op", "grep"}, {"name", "probe"}, {"pattern", "beta"}});
            check(grepped.ok &&
                      grepped.output.find("notes.txt:2:") != std::string::npos,
                  "self-made-tool greps every file in the tool folder");

            const action::Result edited = toolCall(
                {{"op", "edit"}, {"name", "probe"}, {"path", "notes.txt"},
                 {"old", "beta"}, {"new", "gamma"}});
            {
                std::ifstream in(tool + "/notes.txt");
                std::stringstream body;
                body << in.rdbuf();
                check(edited.ok &&
                          body.str().find("gamma") != std::string::npos,
                      "self-made-tool edits a file inside the tool folder");
            }

            const action::Result listed = toolCall(
                {{"op", "list"}, {"name", "probe"}});
            check(listed.ok &&
                      listed.output.find("notes.txt") != std::string::npos,
                  "self-made-tool lists the files of one tool");
            const action::Result all = toolCall({{"op", "list"}});
            check(all.ok && all.output.find("probe/") != std::string::npos,
                  "self-made-tool lists every installed tool");

            {
                action::Request draft;
                draft.name = "write_file";
                draft.args["path"] = "draft.txt";
                draft.args["content"] = "workspace draft\n";
                check(action::execute(draft).ok,
                      "creates a workspace draft for the import test");
            }
            const action::Result imported = toolCall(
                {{"op", "import"}, {"name", "probe"}, {"source", "draft.txt"},
                 {"path", "copied/draft.txt"}});
            check(imported.ok &&
                      ::access((tool + "/copied/draft.txt").c_str(), R_OK) == 0,
                  "self-made-tool imports a workspace draft into the tool");

            const action::Result deleted = toolCall(
                {{"op", "delete"}, {"name", "probe"}, {"path", "notes.txt"}});
            check(deleted.ok &&
                      ::access((tool + "/notes.txt").c_str(), F_OK) != 0 &&
                      ::access(tool.c_str(), F_OK) == 0,
                  "self-made-tool deletes one file but keeps the tool folder");

            const action::Result escaped = toolCall(
                {{"op", "write"}, {"name", "probe"}, {"path", "../escape.txt"},
                 {"content", "no"}});
            check(!escaped.ok &&
                      ::access((tools + "/escape.txt").c_str(), F_OK) != 0,
                  "self-made-tool refuses a path that leaves the tool folder");

            std::string reason;
            if (sandbox::platform().available(reason)) {
                const action::Result ran = toolCall(
                    {{"op", "run"}, {"name", "probe"}, {"path", "run.py"}});
                check(ran.ok && ran.output.find("\n1\n") != std::string::npos,
                      "self-made-tool runs a script from the tool folder");
                const size_t saved = ran.output.find("saved: ");
                if (saved != std::string::npos) {
                    const size_t end = ran.output.find('\n', saved);
                    const std::string path = ran.output.substr(
                        saved + 7, end == std::string::npos
                                       ? std::string::npos
                                       : end - (saved + 7));
                    if (path.find("/runs/") != std::string::npos)
                        std::remove(path.c_str());
                }
                ::rmdir((cfg + "/workspace/runs").c_str());
            }

            // Without a manifest, existing tool folders are adopted once and
            // the manifest is written for the next startup.
            const std::string legacy = tools + "/legacy";
            std::remove(manifest.c_str());
            ::mkdir(legacy.c_str(), 0700);
            self_made_tools::loadGrants();
            check(!self_made_tools::grantedRoot(legacy).empty(),
                  "existing tool folders are adopted when no manifest exists");
            check(::access(manifest.c_str(), R_OK) == 0,
                  "adopting existing tools writes the grant manifest");
            setenv("AI_CHAT_HOME", oldHome.c_str(), 1);

            std::remove(script.c_str());
            std::remove(manifest.c_str());
            std::remove((tool + "/notes.txt").c_str());
            std::remove((tool + "/copied/draft.txt").c_str());
            ::rmdir((tool + "/copied").c_str());
            std::remove((cfg + "/workspace/draft.txt").c_str());
            ::rmdir((cfg + "/workspace/runs").c_str());
            ::rmdir((cfg + "/workspace").c_str());
            ::rmdir(legacy.c_str());
            ::rmdir(tool.c_str());
            ::rmdir(tools.c_str());
            ::rmdir(cfg.c_str());
            ::rmdir((home + "/.config").c_str());
            ::rmdir(home.c_str());
        }
    }

    {
        std::string reason;
        const bool available = sandbox::platform().available(reason);
        check(available || !reason.empty(),
              "sandbox backend reports availability or a reason");
        if (available) {
            const std::string ws = action::workspacePath();
            ::mkdir(ws.c_str(), 0700);
            const std::string script =
                ws + "/selftest-binary-" + std::to_string(::getpid()) + ".py";
            action::Request wr;
            wr.name = "write_file";
            wr.args["path"] = script;
            wr.args["content"] =
                "import sys\n"
                "sys.stdout.buffer.write(b'\\x00\\x01hello')\n";
            check(action::execute(wr).ok,
                  "writes a script with binary output for the runner test");

            action::Request rr;
            rr.name = "sandbox-run-script";
            rr.args["path"] = script;
            const action::Result ran = action::execute(rr);
            check(ran.ok &&
                      ran.output.find("[binary output:") != std::string::npos &&
                      ran.output.find("\\x00") != std::string::npos,
                  "binary script output is summarized with an escaped preview");
            const size_t saved = ran.output.find("saved: ");
            if (saved != std::string::npos) {
                const size_t end = ran.output.find('\n', saved);
                const std::string path = ran.output.substr(
                    saved + 7, end == std::string::npos
                                   ? std::string::npos
                                   : end - (saved + 7));
                if (path.find("/runs/") != std::string::npos)
                    std::remove(path.c_str());
            }
            std::remove(script.c_str());

            const std::string envScript =
                ws + "/selftest-env-" + std::to_string(::getpid()) + ".py";
            action::Request ew;
            ew.name = "write_file";
            ew.args["path"] = envScript;
            ew.args["content"] =
                "import os, sys\n"
                "cwd = os.getcwd()\n"
                "ws = os.environ.get('AI_CHAT_WORKSPACE', '')\n"
                "print('cwd=' + cwd)\n"
                "print('ws=' + ws)\n"
                "sys.exit(0 if ws and ws == cwd else 3)\n";
            check(action::execute(ew).ok,
                  "writes a script that checks the sandbox workspace variable");
            action::Request er;
            er.name = "sandbox-run-script";
            er.args["path"] = envScript;
            const action::Result envRan = action::execute(er);
            check(envRan.ok,
                  "the sandbox exports AI_CHAT_WORKSPACE as the working dir");
            const size_t envSaved = envRan.output.find("saved: ");
            if (envSaved != std::string::npos) {
                const size_t end = envRan.output.find('\n', envSaved);
                const std::string path = envRan.output.substr(
                    envSaved + 7, end == std::string::npos
                                      ? std::string::npos
                                      : end - (envSaved + 7));
                if (path.find("/runs/") != std::string::npos)
                    std::remove(path.c_str());
            }
            std::remove(envScript.c_str());
        }
    }

    const std::string manuals = tag_manuals::all();
    check(manuals.find("Instruction Manual for Top-Level Tags") !=
              std::string::npos,
          "loads the top-tag instruction manual");
    check(manuals.find("Instruction Manual for `<auto/>`") !=
              std::string::npos,
          "loads the auto-tag instruction manual");
    check(manuals.find("Instruction Manual for `<timer N/>`") !=
              std::string::npos,
          "loads the timer-tag instruction manual");
    check(manuals.find("Instruction Manual for `<track .../>`") !=
              std::string::npos,
          "loads the track-tag instruction manual");
    check(manuals.find("Instruction Manual for `<lesson>`") !=
              std::string::npos,
          "loads the lesson-tag instruction manual");
    check(manuals.find("Instruction Manual for `<story>`") !=
              std::string::npos,
          "loads the story-tag instruction manual");
    check(ui::systemPrompt(30, "ai", "", "", "", manuals)
                  .find("Protocol manuals") != std::string::npos,
          "offers the tag manuals in the system prompt");
    check(ui::systemPrompt(30, "ai", "", "", "", "", "", "",
                           "- 2026-10-26 | reading | two offsets need one print")
                  .find("Practice digest") != std::string::npos,
          "the practice digest reaches the chat prompt");
    check(ui::systemPrompt(30, "ai", "", "", "", "", "", "", "",
                           "WHO: Rick\nWANTED: a real animal")
                  .find("Story memory") != std::string::npos,
          "story memory reaches the chat prompt");
    check(ui::systemPrompt(30, "ai", "", "", "", "", "", "", "",
                           "WHO: Rick")
                  .find("Live (the current scene)") != std::string::npos,
          "the story layers are named in the prompt");

    {
        // The questions come from manuals/self-cognition.md; AI_CHAT_QUESTIONS
        // picks a different document, which is the easiest form to test.
        const std::string qpath =
            (std::filesystem::path(scratchHome) / "questions.md").string();
        {
            std::ofstream out(qpath, std::ios::trunc);
            out << "# Test questions\n\n1. Who are you?\n";
        }
        setenv("AI_CHAT_QUESTIONS", qpath.c_str(), 1);
        check(cognition::questions() == "# Test questions\n\n1. Who are you?\n",
              "reads the questions from the file in AI_CHAT_QUESTIONS");
        unsetenv("AI_CHAT_QUESTIONS");
    }

    const std::string combined =
        ui::systemPrompt(30, "ai", "identity body", "", "", "",
                         "# Identity tracker (authoritative)\n"
                         "[Human] Long (human-long) — role: my user\n");
    check(combined.find("identity body") != std::string::npos &&
              combined.find("[Human] Long (human-long)") !=
                  std::string::npos,
          "self-cognition and the identity tracker both reach the prompt");
    check(combined.find("authoritative for objects and facts") !=
              std::string::npos,
          "the prompt marks the tracker as authoritative for entity facts");

    const std::string withLogin =
        ui::systemPrompt(30, "ai", "", "", "", "", "", "alice");
    check(withLogin.find("logged in as alice") != std::string::npos &&
              withLogin.find("not authentication") != std::string::npos,
          "the prompt names the current login-id without treating it as auth");

    ui::setColorEnabled(false);
    ui::setTestMode(false);
    ui::setLoginId("");
    check(ui::userPrompt() == "You > ", "normal prompt label");
    ui::setLoginId("alice");
    check(ui::userPrompt() == "You (alice) > ",
          "the prompt shows the login-id");
    ui::setTestMode(true);
    check(ui::userPrompt() == "You (alice, Test Mode) > ",
          "test mode prompt label");
    ui::setTestMode(false);
    ui::setLoginId("");

    if (!scratchHome.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(scratchHome, ec);
    }
    if (oldHome.empty()) ::unsetenv("AI_CHAT_HOME");
    else ::setenv("AI_CHAT_HOME", oldHome.c_str(), 1);

    std::printf(fails ? "\n%d test(s) failed\n" : "\nall tests passed\n",
                fails);
    return fails ? 1 : 0;
}
