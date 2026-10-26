#include "aichat/core/facts/fallback.h"

#include "aichat/core/facts/text.h"

namespace facts {

namespace {

std::string argOf(const std::map<std::string, std::string>& args,
                  const char* key) {
    const auto it = args.find(key);
    return it == args.end() ? std::string() : trimText(it->second);
}

void finish(std::vector<Fact>& out, const std::string& action, int cycle) {
    for (Fact& f : out) {
        if (f.source.empty()) f.source = action;
        if (f.cycle == 0) f.cycle = cycle;
        if (f.check.empty()) f.check = "action";
    }
}

} // namespace

std::vector<Fact> fallback(const std::string& action,
                           const std::map<std::string, std::string>& args,
                           const action::Result& result, int cycle) {
    std::vector<Fact> out = result.facts;

    if (!result.ok) {
        std::string path = argOf(args, "mirror");
        if (path.empty()) path = argOf(args, "source");
        if (path.empty()) path = argOf(args, "path");
        out.push_back(make(FactKind::Failure,
                           "fail:" + action +
                               (path.empty() ? "" : ":" + path),
                           path, compact(firstLine(result.output)),
                           FactStatus::Reported, "action"));
    } else if (out.empty() && action != "think") {
        out.push_back(make(FactKind::Generic, "action:" + action, "",
                           compact(firstLine(result.output)),
                           FactStatus::Reported, "action"));
    }

    finish(out, action, cycle);
    return out;
}

} // namespace facts
