#include "aichat/core/guards.h"

#include "aichat/core/facts/text.h"

namespace guards {

namespace {

std::string actionKey(const std::string& action,
                      const std::map<std::string, std::string>& args) {
    // std::map iterates in key order, so the same arguments always produce
    // the same key regardless of how the model wrote them.
    std::string key = action;
    for (const auto& kv : args) key += "\n" + kv.first + "=" + kv.second;
    return key;
}

Verdict refuse(const std::string& reason) {
    Verdict v;
    v.allow = false;
    v.reason = reason;
    return v;
}

} // namespace

void RepeatGuard::observe(const std::string& action,
                          const std::map<std::string, std::string>& args,
                          int factVersion) {
    key_ = actionKey(action, args);
    version_ = factVersion;
    strikes_ = 0;
}

Verdict RepeatGuard::check(const std::string& action,
                           const std::map<std::string, std::string>& args,
                           int factVersion) {
    if (actionKey(action, args) != key_ || factVersion != version_) {
        strikes_ = 0;
        return {};
    }

    ++strikes_;
    if (strikes_ >= 2) {
        // Still refuse the step: an identical call that changed no fact is
        // wasted. But do not kill the loop here. Stopping is a decision the
        // motivation signal and the step budget make; this guard only says
        // the step is not worth running.
        return refuse("the same action with the same arguments was proposed "
                      "again after two refusals and still changes no fact; "
                      "choose a different action or a different argument");
    }

    return refuse("this action and these arguments were just tried and "
                  "changed no fact; choose a different step");
}

std::vector<std::string> doneEvidence(const std::string& reply) {
    std::vector<std::string> keys;
    const size_t p = reply.find("<done");
    if (p == std::string::npos) return keys;
    const size_t end = reply.find('>', p);
    if (end == std::string::npos) return keys;

    const std::string tag = reply.substr(p, end - p + 1);
    size_t e = tag.find("evidence=");
    if (e == std::string::npos) return keys;
    e += 9;

    char quote = 0;
    if (e < tag.size() && (tag[e] == '"' || tag[e] == '\'')) {
        quote = tag[e];
        ++e;
    }
    size_t close = quote ? tag.find(quote, e)
                         : tag.find_first_of(" \t/>", e);
    if (close == std::string::npos) close = tag.size();

    const std::string value = facts::trimText(tag.substr(e, close - e));
    std::string current;
    int depth = 0;
    for (char c : value) {
        if (c == '(' || c == '[') {
            ++depth;
        } else if ((c == ')' || c == ']') && depth > 0) {
            --depth;
        }
        // Commas inside a note such as "(cycle 29: rc 0, 361 bytes)" must
        // not split one evidence item into a bogus key.
        if (depth == 0 && (c == ',' || c == ';' || c == '\n')) {
            const std::string key = facts::trimText(current);
            if (!key.empty()) keys.push_back(key);
            current.clear();
        } else {
            current += c;
        }
    }
    const std::string last = facts::trimText(current);
    if (!last.empty()) keys.push_back(last);
    return keys;
}

Verdict checkDone(const facts::Store& store,
                  const std::vector<std::string>& evidence) {
    // Named evidence: the model says what proves completion, the store
    // checks that the named fact exists and is verified.
    if (!evidence.empty()) {
        for (const std::string& key : evidence) {
            const facts::Fact* f = store.find(key);
            if (!f) {
                std::string reason =
                    "done evidence names no current fact: " + key;
                if (key.find(':') == std::string::npos)
                    reason += "\nuse a bare fact key copied from the store, "
                              "not a description of the result";
                else if (key.find('(') != std::string::npos ||
                         key.find(')') != std::string::npos)
                    reason += "\nuse the bare fact key exactly as printed in "
                              "the fact store; do not append cycle notes or "
                              "counts in parentheses";
                return refuse(reason);
            }
            if (f->status != facts::FactStatus::Verified)
                return refuse("done evidence is not verified: " + key + " (" +
                              facts::statusName(f->status) + ")");
            if (f->kind == facts::FactKind::Run &&
                f->value.rfind("exit=0", 0) != 0)
                return refuse(
                    "done evidence is a run that did not exit cleanly: " + key);
        }
        return {};
    }

    // No evidence given: fall back to the generic predicate.
    if (facts::trimText(store.doneWhen()).empty()) return {};
    if (store.hasVerifiedOutput()) return {};
    // No facts at all means the target never used a tool: a conversational
    // target, where "done" stays the model's judgment.
    if (!store.hasFacts()) return {};

    return refuse("the target declares a product but there is no verified "
                  "output fact yet; continue until a run, write, mirror or "
                  "tool is verified, and name it in <done evidence=\"…\"/>");
}

Verdict checkGiveUp(const facts::Store& store) {
    if (!store.hasVerifiedOutput()) return {};

    return refuse("verified output facts exist; inspect the product before "
                  "giving up");
}

} // namespace guards
