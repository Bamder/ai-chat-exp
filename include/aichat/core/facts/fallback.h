#pragma once

#include <map>
#include <string>
#include <vector>

#include "aichat/core/action.h"
#include "aichat/core/facts/fact.h"

// Layer 1 (reduced): compatibility fallback.
//
// Actions report their own typed facts via Result::facts. This layer only
// covers an action that reports none: a Failure on error, a Generic fact
// otherwise, and nothing at all for `think`. It performs no output parsing and
// knows no action names except `think`.
//
// Keeping this small fallback means a new action can never be fact-less.
namespace facts {

std::vector<Fact> fallback(const std::string& action,
                           const std::map<std::string, std::string>& args,
                           const action::Result& result, int cycle);

} // namespace facts
