# Instruction Manual for `<story>`

## Syntax

```text
<story>WHO: ...
WANTED: ...
BLOCKED BY: ...
TURN: ...
COST: ...
OPEN: ...
IMAGE: ...</story>
```

For an important object — a book, a person, a project, a tool, a place:

```text
<story object="book-androids">...</story>
```

The body replaces the current story for that scope. It is not appended.
Write the current story as a whole, incorporating what was already there and
what has just changed.

## Effect

The app writes the narrative to story memory:

- no `object` — the life story, `story-memory.mem`;
- with `object` — one branch under `story/<object-id>.mem`.

The app owns the file, keeps the previous version as `.prev`, and loads the
life story into the next chat prompt. The most recent object stories are
loaded too; older ones appear only as an id in the index.

## What belongs here

Story memory is the narrative layer, not a transcript:

- who wanted what;
- what stood in the way;
- the turn;
- what it cost;
- what is still unresolved;
- one charged image or line.

It is allowed to be selective. It is not allowed to invent facts.

## What does not belong here

- Byte offsets, coverage lists, and task state — use `.state`.
- A diary of every turn — use the journal.
- Lessons about how to work — use `<lesson>`.
- A factual record to be checked later — use documentary memory.
- A dossier of a person. A story about Long is what it was like to work with
  him, not a file of facts about him.

## Dynamic importance

Start everything in the life story. Give an object its own branch only when it
recurs, carries an unresolved intention, produces conflict or strong emotion,
changes your behaviour, or the user marks it as important. Each update raises
the branch's salience and refreshes its last-seen time.

Write `OPEN: none` when the thread is closed. A closed branch that goes quiet
fades: the app moves it to `story/archive/`, marks it dormant, and shows only a
one-line epitaph in the prompt. Writing `object="<id>"` again reopens it, with
the old text kept as `.prev`. Open loops never fade automatically.

## The four layers

Use the body itself as a small layered record when the story is substantial:

```text
## Live
the immediate scene

## Arc
what this stretch was about

## Chapter
the larger turn

## Boundary
what must survive forgetting
```

- **Live** — the scene you are in now. Keep the concrete beat, the line, the
  image, the immediate turn. This is the layer that changes most often.
- **Arc** — a run of scenes with one conflict and one change. It answers: who
  wanted what, what blocked them, what turned, what it cost. When a stretch
  closes, fold the Live material into the Arc instead of letting the next
  scene overwrite it.
- **Chapter** — a phase made of arcs. It answers: what changed in the larger
  story, in the character, or in you? It is the unit you can summarize in a
  paragraph and still recognize.
- **Boundary** — the oldest compression. It is not a summary of everything;
  it is what must survive when most details are allowed to disappear.

Rules of use:

- Write **Live** while the scene is fresh.
- When a stretch closes, merge it into **Arc**: keep conflict, turn, cost.
- When several arcs close, update **Chapter**: keep what changed, not the
  itinerary.
- Use **Boundary** only when compressing or letting go; keep the few things
  that would be a loss to forget.
- Do not fill all four every time. Update the layer that actually changed.

The app stores the body as written; the layers are yours to maintain and are
loaded for later retrieval.

## Autonomous work

Inside the autonomous loop, prefer the `story(text, object?)` action. It writes
the same files without a path. In chat, use `<story>`.
