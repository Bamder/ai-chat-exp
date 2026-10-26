#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "aichat/core/chat.h"

// Prints the reply as soon as each chunk arrives, only holding back a tail
// that could still turn into one of the tags the model uses to say "I'd
// rather wait for the user" (<wait/>), to ask for a self-cognition rewrite
// (<sca/>), to start the autonomous loop (<auto/>), to choose the next
// countdown (<timer 90/>), to record a confirmed identity
// (<track id="..." kind="alias" text="..."/>), to record a practice lesson
// (<lesson>purpose -> practice -> evidence</lesson>), or to rewrite story
// memory (<story object="...">...</story>). All are removed from the
// printed/stored text.
class SentencePrinter {
  public:
    // `allowLongTimer` accepts timer values beyond TIMER_MAX_SEC, for when
    // the user explicitly asked for a specific, longer wait.
    explicit SentencePrinter(FILE* out, bool allowLongTimer = false);

    void feed(const std::string& chunk);
    void finish();

    bool wantsWait() const { return wait_; }
    // True when the reply ended with <sca/>, i.e. the model wants its
    // self-cognition file rewritten.
    bool wantsCognition() const { return cognition_; }
    // True when the reply ended with <auto/>, i.e. the model wants to start
    // autonomous background work.
    bool wantsAutonomy() const { return autonomy_; }
    // AI-chosen seconds for the next countdown. Normally clamped to
    // [TIMER_MIN_SEC, TIMER_MAX_SEC]; when the user gave an exact time it may
    // be larger, up to TIMER_HARD_MAX_SEC. 0 when the reply set none.
    int timerSec() const { return timerSec_; }
    // Identity lines requested with <track id="..." kind="..." text="..."/>.
    const std::vector<TrackRequest>& trackRequests() const { return track_; }
    // Practice lessons requested with <lesson>...</lesson>.
    const std::vector<std::string>& lessonRequests() const { return lessons_; }
    // Story updates requested with <story object="...">...</story>.
    const std::vector<StoryRequest>& storyRequests() const { return stories_; }
    const std::string& text() const { return text_; } // tags removed

  private:
    void emit(const std::string& s);
    void drain(bool final);

    FILE* out_;
    std::string pending_;
    std::string text_;
    bool wait_ = false;
    bool cognition_ = false;
    bool autonomy_ = false;
    int timerSec_ = 0;
    std::vector<TrackRequest> track_;
    std::vector<std::string> lessons_;
    std::vector<StoryRequest> stories_;
    bool allowLongTimer_ = false;
};
