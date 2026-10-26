#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Story memory: the narrative layer, separate from documentary memory.
//
//   <config>/story-memory.mem
//       The life story: the through-line, current era, active arcs and the
//       few object stories that matter right now. This is the trunk.
//
//   <config>/story/<object-id>.mem
//       One branch for an important object: a book, a person, a project, a
//       tool, a place. A branch exists only when the AI decides the object
//       has become important enough to carry its own arc.
//
// Documentary memory answers "what happened" factually. Story memory answers
// "what was this a story of": who wanted what, what blocked them, what turned,
// what it cost, what remains open. It is allowed to be selective and
// interpretive, but it must not invent facts. The app owns the files; the AI
// supplies the narrative through the <story> tag or the story(text) action.
namespace story {

// Directory holding object stories.
std::string rootPath();

// The life-story file.
std::string lifePath();

// Filesystem-safe id for an object name.
std::string objectId(const std::string& object);

// One object branch.
std::string objectPath(const std::string& object);

// Reads the current life story body (header stripped).
std::string loadLife();

// Reads one object story body (header stripped).
std::string loadObject(const std::string& object);

// Replaces the life story with `body`, keeping the previous version as
// story-memory.mem.prev.
bool writeLife(const std::string& body);

// Replaces one object story, keeping the previous version as <id>.mem.prev.
bool writeObject(const std::string& object, const std::string& body);

// Active object ids, highest salience first.
std::vector<std::string> listObjects();

// Moves quiet, closed branches to story/archive/ and marks them dormant.
// Open loops never fade; a high score delays fading but does not prevent it
// forever.
void sweep();

// A bounded prompt block: the life story plus the most recent object branches
// and an index of the rest. Empty when nothing has been recorded.
std::string renderForPrompt(std::size_t maxChars = 5000);

} // namespace story
