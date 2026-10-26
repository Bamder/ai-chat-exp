#pragma once

#include <vector>

#include "aichat/core/facts/fact.h"

// Layer 2: verification.
//
// Confirms, downgrades or contradicts the claims reported by actions (or by
// the fallback layer). Only the app can do this: stat() for paths, the
// recorded waitpid result for a run, the session grant for access. Nothing
// here invents a fact.
namespace facts {

void verify(std::vector<Fact>& facts);

} // namespace facts
