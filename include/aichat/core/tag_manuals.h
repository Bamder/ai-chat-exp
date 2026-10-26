#pragma once

#include <string>

// Loads the top-level tag index and the per-tag instruction manuals from
// manuals/tags (or AI_CHAT_MANUALS_DIR). The returned text is inserted into
// the chat system prompt. If the files cannot be found, returns an empty
// string and the prompt uses a compact fallback.
namespace tag_manuals {

std::string all();

} // namespace tag_manuals
