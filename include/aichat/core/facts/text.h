#pragma once

#include <cstddef>
#include <string>

// Small text helpers shared by the fact-engine layers. Keeping them in one
// place means the fallback, the verifier and the store render values the same
// way, and the persistence format has one escaping rule.
namespace facts {

std::string trimText(const std::string& s);

// Collapses a multi-line tool result into one line. Newlines become " | " so
// listings and script output keep their structure. Truncates to `max`.
std::string compact(const std::string& s, size_t max = 1200);

std::string firstLine(const std::string& s);

// Value after "label:" at the start of any line ("exit:", "mirror:", ...).
std::string field(const std::string& text, const std::string& label);

// Payload after the "\n---\n" rule used by script output.
std::string afterRule(const std::string& s);

// Persistence escaping: tabs and newlines become \t and \n.
std::string escapeField(const std::string& s);
std::string unescapeField(const std::string& s);

} // namespace facts
