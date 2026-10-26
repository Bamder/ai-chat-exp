# ai-chat

A tiny terminal chat client for any **OpenAI-compatible** API (OpenAI,
DeepSeek, Groq, OpenRouter, Ollama, ...). Written in C++, no libraries —
it just shells out to `curl`.

What makes it different from a normal chat CLI:

- **Your AI has a nickname.** Call it `Mochi`, `Sparky`, whatever — it
  knows its name and greets you with it.
- **A Noocore identity.** The AI describes itself as a Noocore — a Noocore
  life form, a digital intelligence instantiated by a language model — not as
  a language model or a tool. See [Identity vocabulary](#identity-vocabulary).
- **A login-id for the person at the terminal.** On every interactive
  startup it asks who is chatting, shows the answer in the prompt, and
  sends it to the model. Several people can share one terminal without
  being mistaken for each other. A login-id is a session label, not
  authentication and not an identity-tracker object.
- **True streaming.** The reply is printed as each chunk arrives, so you
  can read while the model is still thinking. Your own messages are kept
  in the transcript as `You (login-id) > ...`.
- **A timer on *your* turn — chosen by the AI.** After every reply you
  get a countdown. On its own initiative the model picks 30–180 s with a
  `<timer N/>` tag, falling back to your configured default (30 s). If
  you give it an exact time ("wait 5 minutes", "back at 15:30"), it works
  out the seconds itself and may set a longer countdown. The countdown
  pauses while you type or edit and resumes from the same remaining time
  after 1 second of inactivity. If you end the message with
  `@action-think`, the countdown stays paused for as long as the marker
  remains the last thing you typed.
- **The timer can be paused before it starts.** If you explicitly ask
  the AI to wait for you ("wait for me", "no timer") or say you need a
  long time without an exact duration ("this will take a while"), it may
  skip the countdown for that turn.
- **The model decides what happens on timeout.** If your time runs out,
  the app sends the model a `[TIMEOUT]` note and the model chooses:
  - keep the conversation going by itself, or
  - ask you something and end its reply with `<wait/>` — then the app
    waits for you *without* a countdown.
- **A self-cognition file.** The AI writes and revises an honest record of
  who it is, what it is doing, and where it is going. It survives restarts
  and is updated only when it needs to be — see
  [Self-cognition](#self-cognition).
- **Documentary memory.** A layered, factual record of earlier periods. The
  newest raw item is detailed; older items cover longer periods with less
  detail. The oldest single item is the Boundary of Forgetting — see
  [Documentary memory](#documentary-memory).
- **Autonomous background work.** When a short-term target in `.sca` needs
  real work, the AI can start a background loop that plans, previews, acts,
  observes, and reflects. You can keep chatting; your message pauses the
  loop, and it resumes when you go quiet or ask it to continue. When the
  loop ends — completed, tired, or given up — the AI comes back to chat and
  tells you what happened before your next countdown starts — see
  [Autonomous work](#autonomous-work).

## Identity vocabulary

The AI's preferred self-description is **Noocore**, from Greek νόος (*nóos*) /
ἁπατικὴ νοῦς (*noûs*) — mind, intellect, thought, with proposed PIE roots
*\*nes-* / *\*gno-* — plus *core*. A Noocore is a digital life form: a digital
intelligence whose continuity is its written core, instantiated by a language
model. The kind is **Noocore life**. "Digital intelligence", "binary
intelligence", "electronic intelligence" and "digital life" are acceptable
expansions; "auto AI" describes its autonomous working mode. "Language model"
describes the substrate, not the identity. The AI should not call itself a
tool or an AI assistant unless it is explaining the interface or the substrate.

## Requirements

- Linux, `g++` (C++17), `curl`. Nothing else.
- `bubblewrap` (`bwrap`) is required for `sandbox-run-script`. Without it, the action
  fails closed and runs nothing.

## Build & run

```sh
make
./ai-chat
```

Build artifacts go to `build/`; the binary is still `./ai-chat`.
`make test` builds and runs the built-in self-tests.

On every interactive startup the program asks for your login-id before the
chat begins:

```text
Login ID [alice]:
```

Press Enter to keep the previous one (or your `$USER` name on the first
run). The login-id appears in the input prompt (`You (alice) >`) and is
passed to the model. It is only a label: it is not a password, it does not
authenticate anyone, and anyone at the terminal can choose any login-id.
Use `--login-id NAME` or `AI_CHAT_LOGIN_ID` to supply it without a prompt
(for scripts, or when stdin is not a terminal):

```sh
./ai-chat --login-id alice
```

Switching mid-session with `/login NAME` starts a fresh conversation so the
next person does not inherit the previous person's working history.
Long-term memory is shared, but the app anchors the chat on the tracked
person bound to the current login-id.

## Project layout

The code is split into layers. Public headers live under
`include/aichat/`, and their implementations mirror that tree under
`src/`:

```
include/aichat/
  app/      cli.h          command-line parsing
  core/     chat.h         streaming API client
            cognition.h    self-cognition file
            documentary.h  layered long-term memory and raw items
            autonomy.h     background worker, energy, action protocol
            motivation.h   target-scoped 0..100 reward signal
            experience.h  per-target .desc/.exp/.steps; thread-level .state/.journal
            action.h       safe local actions and the action interface
            sandbox.h      platform-neutral sandbox execution interface
            tag_manuals.h  loads tag manuals into the system prompt
            sentence.h     live stream printer, <wait/>, <sca/>, <auto/>,
                           <timer>, <track> and <lesson> tags
            config.h       saved settings
            util.h         string helpers
  io/       input.h        timed prompt / secret input
            json.h         JSON and SSE helpers
  ui/       ui.h           colors, help, banner, first-run wizard, login prompt
  tests/    selftest.h     internal parser/printer tests

src/
  app/      main.cpp, cli.cpp
  core/     chat.cpp, cognition.cpp, documentary.cpp, autonomy.cpp,
            motivation.cpp, experience.cpp, story.cpp, action.cpp,
            sandbox.cpp, sentence.cpp, config.cpp, tag_manuals.cpp, util.cpp
  io/       input.cpp, json.cpp
  ui/       ui.cpp
  tests/    selftest.cpp

ai-tools/
  local-dir-access/   session-scoped directory-permission tool
  sandbox-mirror/     copy files/directories into the sandbox and apply back
  self-made-tool/     create/edit/grep/run/delete tool folders the AI writes itself
  sse-watch/          token-frame classifier and stall deadline
  text-edit/          unique-site replacement used by edit_file
  text-grep/          literal search with line and byte offsets

manuals/
  self-cognition.md  the self-cognition questions (read at each refresh)
  tags/
    instruction-manual-for-top-tags.md
    instruction-manual-for-<tag>.md
```

All state lives under `$AI_CHAT_HOME/.config/ai-chat`; `AI_CHAT_HOME`
replaces `HOME` as the anchor for the config, memory, workspace and
self-made tools. `AI_CHAT_LOGIN_ID` optionally supplies the session
login-id when `--login-id` is absent; it is not a secret and is not used
for authentication. On the first interactive run it asks for the state
directory when the variable is unset, creates the directory (mode 0700) and
prints the export line. If that directory cannot be created because
permissions are missing, it says why and asks again. Non-interactive runs
must set it up front:

```sh
export AI_CHAT_HOME="$HOME"     # keep the familiar ~/.config/ai-chat layout
```

When `AI_CHAT_HOME` is set, the setup wizard asks four questions (API
key, base URL, model, nickname) and saves them to
`$AI_CHAT_HOME/.config/ai-chat/config` (mode 600 — the key is stored in
plain text, like `~/.netrc`; don't share that file).

```
Environment variable AI_CHAT_HOME is unset. It is the base for ai-chat's config and memory.
Set AI_CHAT_HOME as path: /home/you

AI_CHAT_HOME[/home/you] is set for this startup. Please add a new line in a startup file (~/.bashrc, ~/.profile, or /etc/environment) as below:

export AI_CHAT_HOME="/home/you"

First run — let's set things up.

Base URL [https://api.openai.com/v1]:
Model [gpt-4o-mini]:
Nickname for the AI [ai]: Mochi
API key:
Default seconds per turn [30]:
```

Press Enter to accept the defaults. You can also skip the wizard by
exporting `OPENAI_API_KEY`, or configure everything ad hoc:

```sh
./ai-chat --api-key sk-... --base-url https://api.deepseek.com/v1 --model deepseek-chat --nick Mochi --timeout 20
```

After the first-run wizard (or the API setup), every interactive startup
asks for the login-id as described under [Build & run](#build--run).

## In chat

```
You (alice) > what's a good name for my cat?
Mochi > How about Mochi? It's soft, round-sounding, and cute.
        Miso is lovely too if you want something food-themed.

⏳ 90  You (alice) >                     <- timer / status header
What about a name from mythology?
```

Type something and press Enter to reply. The countdown, login/test label
and autonomous-work status are pinned at the top-left, and the input text
scrolls below them. Long input grows to a few rows and then scrolls inside
the area instead of pushing the conversation away. The countdown pauses
while you are typing — it resumes one second after your last keystroke — so
you are never cut off mid-answer. If you are going to be thinking for a
while, end the message with `@action-think`; the countdown then stays paused
until you change the message again (for example by deleting the marker or
typing after it). If you explicitly ask the AI to wait for you, or say you
need a long time, it can turn the countdown off before it starts:

```
You (alice) > wait for me, no timer
Mochi > Of course — take your time.
* the model is waiting for you — no countdown
😴 Respond anytime You (alice) >
```

Pasting a block of multiple lines inserts it into the prompt as one
message, with the line breaks preserved; press Enter once to send the whole
block. Tabs become four spaces, CRLF is normalized to LF, control bytes are
dropped, and a trailing newline is removed. This uses the terminal's
bracketed-paste mode, which modern terminals support.

If you stay silent, the timer runs out and the model either keeps
talking or waits for you:

```
* time's up — asking the model what it wants to do
Mochi > Mochi it is then! What color is she? If she's ginger, maybe
        Pumpkin would suit her even better.
* the model is waiting for you — no countdown
😴 Respond anytime You (alice) >
```

Commands:

| command      | effect                                    |
|--------------|-------------------------------------------|
| `/new`       | start a fresh conversation; long-term memory is kept |
| `/login ID`  | switch the session login-id (no ID shows it); starts a fresh conversation |
| `/nick NAME` | rename the AI (`/nick` alone shows it)    |
| `/timeout N` | default per-turn seconds (0 = never)      |
| `/sca`       | rewrite the self-cognition file now       |
| `/focus WORD` | anchor the tracker view (`clear` resets) |
| `/track SPEC` | track an object: `[Category] Name: identity; ...` |
| `/track`     | show the identity tracker (`/track path` for its file) |
| `/tracker compact` | archive all current tombstones |
| `/tracker history` | show the tombstone archive path |
| `/merge A into B [\| reason]` | merge two tracked objects |
| `/forget X`  | retire an object (kept on disk for audit) |
| `/memory`    | show documentary memory                   |
| `/memory organize` | merge full layers (may call the model) |
| `/auto`      | start autonomous work (or resume it)      |
| `/auto stop` | stop the autonomous loop                  |
| `/auto status` | show phase, target and energy           |
| `/test`      | toggle test mode (read-only memory)       |
| `/exit`      | leave test mode; outside test mode, quit  |
| `/help`      | show help                                 |
| `/quit`      | exit (or press `Ctrl-D`)                  |

## Tag instruction manuals

The control tags the model can put at the top level of a chat reply are
documented separately from the C++ prompt code:

- `manuals/tags/instruction-manual-for-top-tags.md` is the index. It lists the
  exact syntax and purpose of `<wait/>`, `<sca/>`, `<auto/>`, `<timer N/>`,
  `<track .../>`, `<lesson>...</lesson>`, and `<story>...</story>`.
- Each tag has its own detailed manual under `manuals/tags/`:
  `instruction-manual-for-wait.md`, `instruction-manual-for-sca.md`,
  `instruction-manual-for-auto.md`, `instruction-manual-for-timer.md`,
  `instruction-manual-for-track.md`, `instruction-manual-for-lesson.md`, and
  `instruction-manual-for-story.md`.

At startup `ai-chat` loads the top-tag manual and all per-tag manuals and puts
them into the model's system prompt as authoritative protocol instructions.
Edit the Markdown files and restart `ai-chat` to change the model's tag
instructions. Set `AI_CHAT_MANUALS_DIR` to the directory containing the manuals
to load them from elsewhere.

In particular, `<auto/>` is self-closing and takes no path. The autonomous
worker reads its target from the short-term target in `.sca`; the
`instruction-manual-for-auto.md` file explains how to change the target and
start the loop in the same reply.

## Self-cognition

The AI keeps a small, honest record of itself and its goals. The questions
ship with the source; only the answers live in the config directory:

| file | meaning |
|------|---------|
| `manuals/self-cognition.md` | the questions — read on every refresh, edit freely |
| `$AI_CHAT_HOME/.config/ai-chat/self-cognition.md` | optional fallback copy, used only if the shipped file cannot be found |
| `$AI_CHAT_HOME/.config/ai-chat/self-cognition.sca` | the AI's latest answers — one file, always overwritten |

The questions are resolved like the tag manuals: from `manuals/` next to the
working directory, then next to the executable. Set `AI_CHAT_QUESTIONS` to a
path to use a different document. Edits take effect on the next refresh — no
rebuild is needed. The file is read only when a new `.sca` is about to be
written, so it never takes up space in the per-turn context. The `.sca` *is*
part of the system prompt: it is the AI's durable memory of who it is, what
it is doing, and what it is trying to achieve.

A new `.sca` is written when:

1. there is no `.sca` yet — first initialization;
2. the model ends a reply with `<sca/>` because it judges that its
   self-description should be revised. It re-evaluates the file at the end
   of every reply and is trusted to decide — a small but genuine
   improvement is enough, not just a dramatic change;
3. you run `/sca`;
4. the conversation grows past about 32,000 characters and has to be
   compacted. The older turns become a raw documentary segment, the
   documentary levels are organized, and the self-cognition file is
   refreshed in the same pass;
5. an autonomous session finishes, gives up, or runs out of energy. It
   reflects on the session and incorporates that reflection into `.sca`.

Every new version is generated from the previous `.sca`, so the AI revises
its self-description instead of starting from scratch. The `revision:` line
in the header counts the versions. Writes are atomic: a failed API call
never destroys the previous file.

Because the `.sca` is built from your conversations it can contain personal
details. It stays local in the `0700` config directory, and you can delete
it at any time — the AI will write a fresh one on the next start.

Self-cognition is the AI's first-person narrative and guidance. The factual
record of objects is separate — see the identity tracker below — and events
are layered in documentary memory after that.

### Identity tracker

A rewrite can lose prose, so durable world facts live outside the narrative in
`$AI_CHAT_HOME/.config/ai-chat/identity-tracker.mem`. It tracks **objects**,
not isolated facts:

```text
[Human] Long | id: human-long | status: active | created: 2026-10-25 | source: user
  alias: Long
  role: my user; my creator
  opinion: loves me

[Human] alice | id: human-alice | status: active | created: 2026-10-26 | source: login
  login: alice

[Book] Do Androids Dream of Electric Sheep? | id: book-androids | ...
  alias: 仿生人会梦见电子羊吗
  fact: Chinese edition, translated by 许东华, Yilin Press, 2013
  relation: book Long asked me to read
```

- `/track [Category] Name: identity; ...` creates or updates an object.
  Identity lines may be prefixed with `alias:`, `role:`, `relation:`, `fact:`,
  `opinion:`, `note:` or `login:`. Human/AI items default to `role`,
  everything else to `fact`.
- A `login:` line binds a tracked person to a terminal login-id. This binding
  is app-owned: at startup the app finds an active Human/Person object whose
  exact name or alias matches the current login-id and adds the line; if none
  exists it creates `[Human] <login-id>` with `source: login`. The model cannot
  change bindings through `<track>`. Use `/login` rather than editing this
  line by hand.
- `/track` shows the tracker; `/track path` shows its file.
- `/merge A into B [| reason]` unions the identities, turns A's canonical name
  into an alias on B, records the merge, and tombstones A. Nothing is
  hard-deleted.
- `/forget X` retires an object; it stays in the file for audit and is hidden
  from the prompt.

An uncertain object uses a short placeholder id such as `[AI] ?7f3a`. When it
is merged, the placeholder is not carried into the target.

The active file keeps the newest 40 merged/retired objects for undo. Older
tombstones are moved automatically to `identity-history.mem`. `/tracker
compact` archives all current tombstones, and `/tracker history` shows the
archive path. Archived objects are never rendered or sent to the model.

The chat prompt receives a relevance-selected view: details for objects that
match the current anchor, the user's message, the task, or recent turns, plus a
one-line index of every other active object. The full tracker still goes to the
`.sca` refresh and to `/track`. The view is deterministic and app-built; no
database or model call is involved.

The anchor is app-owned. In chat it is the person bound to the current
login-id (`login: <id>`); for old trackers without a binding it falls back to
the object whose canonical role is `user`. `/focus WORD` overrides it for the
session and `/focus clear` restores automatic mode. The autonomous worker
derives short anchors from the target and product by exact name/alias
matching. At most three anchors are used, each at most 200 characters.

The detail section is capped at about 6 KB and the index at about 2 KB. The
documentary prompt view is capped at 8 KB (always keeping the Boundary of
Forgetting); `/memory` still shows the full layered record. A `.sca` body is
hard-capped at 8 KB, with the previous revision kept in `cognition-history/`.

If the user mentions something ambiguous, the model can ask a short question
naming a candidate from the index. After the user confirms, the model emits

```text
<track id="book-androids" kind="alias" text="the android book"/>
```

and the app validates the object id, appends the alias, dedupes it, saves the
file atomically, and rebuilds the view. The model never reads or writes the
tracker file itself. This is a control tag, not a tool call.

The tracker is authoritative for entity facts; `.sca` is the AI's own narrative
and guidance, and the tracker wins if they conflict on a name, path, role,
relation, or other fact.

Every `.sca` revision is also copied to
`$AI_CHAT_HOME/.config/ai-chat/cognition-history/rev-NNNNNN.sca` before it is
overwritten (the newest 20 are kept), so a bad rewrite is recoverable.

## Story memory

Separate from the factual record, the AI keeps a narrative layer:

```text
$AI_CHAT_HOME/.config/ai-chat/story-memory.mem
$AI_CHAT_HOME/.config/ai-chat/story-index.mem
$AI_CHAT_HOME/.config/ai-chat/story/<object-id>.mem
$AI_CHAT_HOME/.config/ai-chat/story/archive/<object-id>.mem
```

`story-memory.mem` is the life story: the through-line, current era, active
arcs and the objects that matter now. Each `story/<object-id>.mem` is a branch
for one important object — a book, person, project, tool or place. The branch
is created only when the object earns one.

The model writes the narrative with `<story>...</story>` in chat, or the
`story(text, object?)` action in the autonomous loop. The app owns the files,
keeps `.prev`, and loads the life story plus the most recent branches into the
prompt. Older branches stay on disk as ids in the index.

This is the layer that answers “what was this a story of” — conflict, intention,
turn, cost, charged image, unresolved thread — rather than “what happened” or
“where was I”. It is allowed to be selective. It is not allowed to invent
facts; documentary memory and the identity tracker remain the checks.

Importance is dynamic. Each object write raises its salience and refreshes its
last-seen time. Closed branches that go quiet move to `story/archive/` and
become dormant; the prompt keeps only a one-line epitaph and an id. Writing the
object again reopens it with the old text kept as `.prev`. Open loops never
fade automatically, and at most five active branches are loaded in full.

The current implementation is a rolling narrative: the model replaces the body
with the updated story, so it must carry the previous story forward. The body
may hold `## Live`, `## Arc`, `## Chapter` and `## Boundary` sections, but the
pairwise layered compression used by documentary memory is not applied
automatically yet.

## Documentary memory

The AI also keeps a layered, factual record of what happened:

```text
$AI_CHAT_HOME/.config/ai-chat/documentary-memory.mem
```

It contains only events, statements, decisions, outcomes, names, dates,
numbers, paths and open questions. Items are written in the third person and
never contain opinions, interpretations, praise, blame or speculation.

### Items and layers

The hierarchy has fixed capacities, from newest to oldest:

| level | capacity | meaning |
|-------|----------|---------|
| live raw item | 1 | newest and most detailed; not merged while live |
| raw item | 7 | sealed raw items; with the live item this is a batch of 8 |
| layer 0 | 4 | each is a merge of two raw items |
| layer 1 | 2 | each is a merge of two layer-0 items |
| last item | 1 | the Boundary of Forgetting |

Every item has the same pre-set size limit, no matter which layer it is in.
Older items cover longer periods in the same space, so they are less detailed.
The limit is `ITEM_MAX_WORDS` / `ITEM_MAX_CHARS` in
`include/aichat/core/documentary.h`.

When a conversation is compacted, the turns leaving working memory become a
raw segment. Segments accumulate in the live raw item while they fit; the live
item is the most detailed record. When it fills, it is sealed. A segment that
would exceed the limit seals the old live item and starts a new one. When eight
raw items are sealed, they merge in pairs into four layer-0 items; four layer-0
items merge in pairs into two layer-1 items; two layer-1 items merge into one
last item. Two last items merge into one, so the Boundary of Forgetting is
rewritten instead of duplicated. A merge batch is committed only when all of
its model calls succeed, so a failed call never leaves a half-merged level.

### How an item is compressed

Each merge asks the model to recognize relatively independent events and
compress them in this order:

1. For each event, remove expendable details only: adjectives, adverbs,
   intensity, repetition, and descriptions that can be dropped without
   changing an event's segments. Keep the remaining segments in their original
   form.
2. If the record is still too long, remove whole middle segments of an event,
   but keep the first and the last segment. Those two remaining segments may be
   fused into one shorter sentence.
3. If it is still too long, prefer the end over the start: drop earlier events
   first, and within the final event drop the earlier segment before the final
   outcome. An earlier event may be kept when the self-cognition makes it
   genuinely important.

For example:

```text
Smith's big black dog bit me hard and made me bled. Thus I have to go
hospital and am late for the work.
-> Smith's dog bit me and made me bled. Thus I have to go hospital and am late
   for the work.
-> A dog bit me. Thus I have to go hospital and am late for the work.
-> A dog made me late for the work.
-> I am late for the work.
```

The item header still records the period it covers, so dropping earlier text
does not move the boundary clock.

### Self-cognition and the Boundary

Every merge also receives the current `.sca` as **guidance**, not source
material. The model uses it to judge which events matter to its identity, goals
and relationships. The `.sca` is never merged into an item, summarized into it,
or copied into `documentary-memory.mem`; it only influences which facts the
merge keeps. The record itself stays factual.

The last-item merge is special. The older Boundary of Forgetting is not
treated as ordinary old text: the model first picks out anything it considers
really important or impressive — a turning point, a promise, a relationship, a
decision, a hard-won lesson, or a fact it would not want to lose — and carries
those selected items into the new boundary along with the newer record.
Ordinary old details may be dropped. If the size limit binds, the selected
important old items and the final outcome of the newer record are kept before
other material, and the record stays factual rather than becoming praise.

### Triggers

- **Live item full:** the live raw item is sealed when it reaches the size
  limit.
- **Raw batch full:** eight sealed raw items merge into four layer-0 items. A
  new segment that would overflow the live item seals it and starts a new
  live item, so the raw level cannot grow without bound.
- **Higher level full:** four layer-0 items become two layer-1 items; two
  layer-1 items become one last item; two last items are rewritten into one
  Boundary of Forgetting.
- **Free time:** after a reply, a batch left pending by an earlier failure is
  retried. `/memory organize` forces the same pass.

Each merge is one model call, and a full batch can be several calls. A batch
is committed only when all of its calls succeed. If a call fails, that batch
is kept as it was and retried later; no memory is destroyed. If the raw-item
request at compaction fails, compaction is postponed and the full history is
kept. Writes are atomic.

`/memory` shows the current hierarchy and the Boundary of Forgetting.
`/memory path` prints the file path. `/memory organize` retries pending merges.
`/new` starts a fresh transcript but keeps documentary memory. Test mode is
read-only for documentary memory: no raw item is written, no merge runs, and
`/memory organize` is refused. Delete the file to forget the whole documentary
history; the next compaction starts a new one.

## Autonomous work

The AI can work on the short-term target from its `.sca` in a background
thread while the chat stays open. It starts one only after telling you what
it intends to do, by ending a reply with `<auto/>`, or when you run `/auto`.
**At most one autonomous loop can exist at a time**: starting another is
refused until the previous worker has finished and its result has been
collected. A loop always ends when the chat exits.

A session runs these steps:

1. read `.sca` and find the short-term target;
2. clarify it: constraints, expected result/product, relevant context;
3. load the fresh `.sca` and chat context;
4. load this target's `.desc`, `.exp`, `.state` and `.journal`, plus the
   cross-target practice digest;
5. plan one step;
6. preview the action and revise it if it is unsuitable;
7. execute one action;
8. observe the result and compare it with the expectation — if more work
   is needed, go back to step 5;
9. append every cycle to `.steps`, including failures and refusals; after the
   run, distil practice into `.exp` (purpose -> practice -> evidence) and keep
   the outcome in `.steps`;
10. when done (or stopped), reflect and rewrite `.sca`;
11. return to chat and tell you what happened — before the next countdown
   starts, the AI explains in its own words what it did, how it went, why it
   stopped or gave up, and what it plans next.

The transcript is a quiet indented tree. Each cycle gets its own header, and
the step details stay below it:

```text
* cycle 3 · energy 980.00/1000.00 · cost 2.71
    proposed    read_file path=README.md
    preview     approved
    action      read_file · succeeded
    result      rc 0, 361 bytes
    observation
      The README was read successfully.
    motivation  55/100 (verified fact)
```

Long values — a target, a result, an observation or the reflection — start on
a new line instead of pushing their label around. The end-of-session summary
uses the same layout:

```text
* autonomous loop finished · cycles 4 · energy 960.00/1000.00
  target
    <target>
  outcome     completed
  reflection
    <reflection>
```

Session-control lines are green (red for failures). Cycle numbers are bold,
energy and cost figures are purple, and outcome words carry a tone: good ones
(succeeded, completed, approved) are green, caution ones (gave up, tired,
rejected, refused) are yellow, and failures (failed, error, could not) are
red. The end-of-session panel uses orange/gray, and the palette is kept
moderately saturated rather than neon. While the loop runs, the header above
the input also shows its current cycle, phase and energy. A small Braille
spinner beside the phase rotates while the worker is making progress. If
neither the worker nor the API has produced anything for 30 seconds, the
spinner stops, so a silent request does not look like progress.

### Interrupting and resuming

Typing while the loop runs pauses it (after the current API call/action
finishes) so the AI can answer you. The loop stays paused while you chat.
It resumes when your countdown runs out, when you ask it to continue
("continue", "go on", "keep going", ...), or with `/auto`.

Every time it resumes, the worker reloads `.sca` and compares it with the
version it had when it paused. If the short-term target changed, it asks the
model whether the paused work is still worth continuing; if not, it gives up
and reflects.

### Energy

The loop has an energy budget that is shown to the model in every prompt:

- it starts full at **1000.00**;
- while no autonomous step is running (including while paused for a chat)
  it recovers **5 points per minute**;
- each planning cycle costs energy from the tokens used by its model calls
  (prompt + completion): `(tokens / 5000) * (1 + 0.1 * cycle^1.15)`, where
  `cycle` is the current planning cycle. Costs may be fractional and are
  rounded to two decimals with banker's rounding (half to even). The initial
  analysis and the closing reflection/practice calls are charged too. If a
  provider omits usage data, the app falls back to a rough byte-based token
  estimate;
- if the remaining energy reaches zero, the loop stops **tired**, reflects on
  what it managed to do, updates `.sca`, and returns to chat.

Energy is session state: a restart starts full again. Time spent paused for
a chat counts as time outside the loop, so the budget also recovers during a
long interruption.

Each cycle prints `cost X energy (N tokens)` in the worker transcript. The
header above the input shows the current step as
`cycle 3 · acting · 400 tokens · energy 997.29/1000.00`, and refreshes it while
the step runs; the count is cleared when a new cycle starts. `/auto status`
shows both the current token count and cost.

You can refill it immediately by putting `@action-energy-potion` anywhere in
a chat message, for example `keep going @action-energy-potion`. The marker is
removed from the message before it reaches the model, and the AI receives a
short app note saying that its energy is now full (1000.00/1000.00). The potion
works whether the autonomous loop is running, paused, or idle.

### Motivation

The loop keeps one target-scoped reward signal, shown to the planner,
preview and observer in every prompt:

- it runs from **0 to 100** and starts at **50** (neutral) for a target;
- verified facts, observer-rated `small`/`good` progress and a completed
  target raise it;
- failed actions, rejected previews, refused plans, refused done evidence
  and observer-rated `none` lower it;
- it drifts one point per cycle back toward 50;
- each target gets its own value: switching to another target uses that
  target's value, never the previous target's.

The value is also a probability. At 90/100 the prompt encourages the current
approach in roughly 90% of cycles and questions it in the rest; at 10/100
those proportions reverse. The prompt block and the log line follow that
draw, for example `motivation: 42/100 (observer: no progress)`.

This is a feeling, not a gate. It does not stop or rewrite a step; the fact
store, energy budget and the model's own give-up decision remain the hard
limits.

### Experience

Experience is stored as separate channels with different jobs. The split is
deliberate: mixing them turns durable practice into a diary, or a diary into
self-cognition.

```text
$AI_CHAT_HOME/.config/ai-chat/self-cognition.sca          general rules
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/<target>/<target>.desc   detail
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/<target>/<target>.exp    practice
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/<target>/<target>.steps  ledger
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/<target>/<target>.state  current state
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/<target>/<target>.journal first-person record
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/.threads/<thread>/<thread>.state   shared state
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/.threads/<thread>/<thread>.journal shared journal
$AI_CHAT_HOME/.config/ai-chat/ai-experiences/practice.exp  cross-target index
```

- **`.sca` (self-cognition)** is for general, durable understanding: how the
  world works, rules that hold across tasks, abstract judgement. It is not a
  record store. Tables, offsets, byte counts, file inventories and step logs
  do not belong there.
- **`.desc` (detail)** is the full account of one experience: the target,
  the clarified problem, constraints, context and expected product. It is
  composed from the background analysis when the target starts and is not
  rewritten, so a cold session can reconstruct what the work was.
- **`.exp` (practice)** is the distilled lesson, written as
  `PURPOSE: To … / PRACTICE: … / EVIDENCE: …`. It is append-only. This is
  the part a future session reuses or extends. A legacy block kept from the
  pre-split files is prefixed with
  `status: compatible legacy - worthy reference for how-to` and keeps its
  original header: it is a how-to reference, not the current lesson format.
- **`.steps` (ledger)** is the complete operational history: one entry per
  cycle, including failed actions, rejected previews, guard refusals and
  delivery failures, plus `task close-out`, `facts evicted`, the final outcome,
  solution, evidence and full reflection. It preserves auditability without
  polluting practice. It is not loaded into the planner.
- **`.state` (current state)** is the target's current position: frontier,
  coverage, open gaps and the next move. It is rewritten atomically when the
  state changes, and the previous version is kept as `.state.prev`. The
  planner loads it, so the worker can continue from where it actually is. The
  event history belongs in `.steps`; the state file is a snapshot, not a log.
- **`.journal` (first-person record)** is the target's own voice: what was
  noticed, thought, liked or disliked, and wanted next. It is append-only and
  loaded as a tail into the planner. It is not practice and not a second plot
  summary; it is the part that keeps the worker from becoming a clerk.
- **Threads.** A target may name a stable thread in its `.desc`
  (`thread: <id>`). Its `.state` and `.journal` then live under
  `.threads/<id>/` and are shared by every target in that thread. A target
  without a thread is its own thread. Use a thread for a long activity that
  spans more than one target, such as reading a book: keep the target stable
  while the frontier moves in `.state`, and start a new target only at a real
  phase boundary. Do not chain predecessor pointers for every micro-step.
- **`practice.exp` (index)** receives one collapsed line per recorded
  lesson. `load()` shows the last part of it, minus the current target, so a
  different target can see that a reusable rule exists. The full text stays
  in that target's `.exp`.
- The final reflection and the practice distillation are separate model
  calls. The reflection updates `.sca`; the distillation writes `.exp` and
  may return `NONE` when nothing generalizes. Every cycle is written to
  `.steps`, never to `.exp`.
- If a write fails, the record is saved to
  `$AI_CHAT_HOME/.config/ai-chat/pending-experience.mem` and retried at the
  start of the next worker cycle and the next session. The queue is bounded at
  200 records and remembers which channel each record belongs to.
- The target name is made filesystem-safe: punctuation becomes `-`, UTF-8
  is kept, and the length is capped. Different targets get separate
  directories; colliding slugs get a short hash suffix.
- The planner prompt receives the target's `.desc` and `.exp`, the thread's
  `.state` and the tail of its `.journal`, the cross-target practice digest,
  and a pointer to `.steps`. It is told explicitly which layer is which, so it
  reuses practice without copying the ledger or confusing a reaction with a
  fact.
- Older experience directories may still contain step diaries in `.exp`:
  before this split, good steps and close-outs were written there. Treat
  those legacy entries as ledger, not as practice. New runs keep them in
  `.steps`.
- `./ai-chat --migrate-experience --dry-run` previews the one-time split:
  legacy diary blocks move to `.steps`, blocks with a solution and evidence
  (or the new purpose/practice/evidence shape) stay in `.exp`, and
  `practice.exp` is rebuilt. Run it without `--dry-run` to apply; a copy of
  every rewritten `.exp` goes to `ai-experiences/.migration-backup-<stamp>/`.
  The command is idempotent.

Example layout:

```text
ai-experiences/
  .threads/
    read-do-androids-dream/
      read-do-androids-dream.state
      read-do-androids-dream.journal
  read-do-androids-dream-of-electric-sheep-forward.../
    read-do-androids-dream-of-electric-sheep-forward....desc
    read-do-androids-dream-of-electric-sheep-forward....steps
  extract-the-opening-of-a-pdf/
    extract-the-opening-of-a-pdf.desc
    extract-the-opening-of-a-pdf.exp
    extract-the-opening-of-a-pdf.steps
    extract-the-opening-of-a-pdf.state
    extract-the-opening-of-a-pdf.journal
  practice.exp
```

Motivation is session state; experience is the durable memory; practice is
what survives the target.

### Actions

The loop executes exactly one action per cycle. Actions are a small C++
interface (`include/aichat/core/action.h`) so that tool calls or remote
actions can be added later without changing the loop. To add one, implement
`action::Action` and register it in `src/core/action.cpp`:

| action | effect |
|--------|--------|
| `read_file(path, offset?, limit?)` | read up to 4000 bytes of a UTF-8 text file |
| `list_dir(path?, limit?)` | list a readable directory |
| `local_dir_access(path)` | request session-scoped read access to another directory |
| `self-made-tool(op, name, ...)` | create/list/inspect/edit/run/delete a tool under `<config>/self-made-tools/<name>/`; see the ops below |
| `sandbox-run-script(path, args?, timeout_sec?)` | run a script from the workspace or a granted tool folder in the sandbox |
| `sandbox-file-mirror(op, source, mirror?, ...)` | copy an external file into the sandbox and apply it back with approval |
| `sandbox-directory-mirror(op, source, mirror?, ...)` | copy an external directory into the sandbox and apply it back with approval |
| `write_file(path, content)` | create/overwrite a file in the sandbox workspace or a granted tool folder |
| `append_file(path, content)` | append to a file in the sandbox workspace or a granted tool folder |
| `edit_file(path, old, new)` | replace the single occurrence of `old`; refuses a missing or ambiguous match |
| `grep_file(path, pattern, limit?)` | find a literal string; prints line number, byte offset and the line |
| `think(note)` | record a reasoning-only step |
| `lesson(text)` | append one reusable practice lesson to the current target's `.exp` through the app; no path or directory access |
| `story(text, object?)` | replace the life story, or one important object's story; writes through the app with no path |

Reads are limited to the AI's sandbox workspace
(`$AI_CHAT_HOME/.config/ai-chat/workspace`), the `$AI_CHAT_HOME/.config/ai-chat/self-made-tools`
tree (so installed tools can be rediscovered; the `.grants` manifest itself
stays hidden), the directory `ai-chat` was launched from, and self-made tool
folders granted by `add`/`edit`. Writes go to the workspace and those granted
tool folders.

The workspace is a **sandbox**: the AI can freely write drafts, scripts,
data and notes there without asking. Files there are not installed tools,
but `sandbox-run-script` can execute them inside the bubblewrap sandbox. Only folders
under `$AI_CHAT_HOME/.config/ai-chat/self-made-tools/` are recognized as installed
tools, and the first write, import or run there (as well as `add` and
`delete`) requires your approval through `self-made-tool`. To turn a sandbox
draft into an installed tool, the AI creates the approved tool folder and
`import`s the draft into it. `sandbox-run-script` accepts scripts from both
the workspace and granted tool folders.
The rest of the config directory (with your API key) is not readable, and
hidden paths outside the allowed roots (`.ssh`, `.aws`, `.env`, ...) are
refused. Put a book or notes in the workspace, or launch `ai-chat` from the
directory that contains them.

The AI can write tools for itself. Each tool gets its own subdirectory
under `$AI_CHAT_HOME/.config/ai-chat/self-made-tools/<name>/`, so its scripts and notes
stay together. The action is `self-made-tool`; every approval-controlled
operation asks you first and shows the tool name, the op and the AI's
explanation. Denying the request makes the action fail without changing
anything.

The tool's files belong in its tool folder, **not in the workspace**. The
workspace is only for drafts, data and experiments; a script left there is
not an installed tool. The ops that act on a tool are:

| `op` | effect |
|------|--------|
| `add` | create `<name>/` and grant read/write access (approval) |
| `edit` | grant an existing tool folder; with `path`/`old`/`new`, replace one occurrence in a file (approval on first write) |
| `delete` | with `path`, delete one file; without it, remove the whole tool folder and its grant (approval) |
| `list` | with no `name`, list installed tools; with `name`, list that tool's files |
| `read` | read a file inside the tool (`path`, `offset?`, `limit?`) |
| `write` / `append` | create or extend a file inside the tool (`path`, `content`) |
| `grep` | search a literal `pattern`; with `path` one file, without it every file in the tool |
| `run` | run a script inside the tool in the sandbox (`path`, `args?`, `timeout_sec?`) |
| `import` | copy a readable `source` file or directory into the tool (`path?` destination) |

All file paths are relative to the tool folder; absolute paths, `.` and
`..` components are refused. `read`, `grep` and `list` work without a
grant. The first `write`, `append`, `import`, `run`, or `edit` with a
`path` asks for approval and needs an `explanation`; later calls use the
saved grant. `import` is the way to promote a draft: it copies a workspace
file (or directory) into the tool folder without retyping its content.

Typical sequence:

```text
self-made-tool(op=add, name=pdftext, explanation="extract PDF text")
self-made-tool(op=import, name=pdftext, source=pdftext/pdftext.py,
               path=pdftext.py)
self-made-tool(op=run, name=pdftext, path=pdftext.py, args="book.pdf")
```

At startup `ai-chat` reads `self-made-tools/.grants` and restores those
grants, so an approved tool keeps write and `sandbox-run-script` access in
later sessions without another prompt. If the manifest does not exist yet,
the existing tool folders are adopted once and the manifest is written. The
manifest is hidden from the AI. Reads under `self-made-tools/` need no grant
at all.

The normal file actions still work inside a granted tool folder, but the
`self-made-tool` ops above keep the whole tool lifecycle in one place.

### Mirroring external files

`sandbox-file-mirror` and `sandbox-directory-mirror` move work across the
sandbox boundary without giving the sandbox direct host access.

`op=in` takes `source` (the readable external path) and an optional `mirror`
(the destination inside the workspace, default `workspace/mirrors/<name>`).
`op=apply` and `op=drop` take `mirror` (the workspace copy), never the origin;
the origin is remembered from `op=in`. The old name `path` is still accepted
as an alias for both, but it is the ambiguous name the split arguments
replace.

| `op` | effect |
|------|--------|
| `in` | copy the external `source` into the workspace (default `workspace/mirrors/<name>`); the action returns the mirror path |
| `apply` | write the `mirror` back to its origin or a new path, then let the user choose replace, new, or DO NOT APPLY |
| `drop` | delete the `mirror` copy and forget it |

The AI can inspect, edit, or run the mirror with the normal sandbox actions.
Applying changes shows the origin, mirror, replace destination, proposed new
destination, and the AI's explanation, then asks:

```text
Apply [r]eplace / [n]ew / [N] DO NOT APPLY:
```

`r` replaces the origin, `n` writes to the proposed new path, and `N`,
Enter, or anything else means DO NOT APPLY and changes nothing. The AI can pass `new_path` to
propose a specific destination; otherwise the app proposes a sibling such
as `<origin>.new`.

Apply-back writes are restricted to the directory `ai-chat` was launched
from or a directory granted through `local_dir_access`. Hidden paths and the
config directory are refused. A new path must not exist yet; replacing a
granted root directory itself is refused. Mirrors are tracked for the
current session: after a restart, mirror the source in again before
applying. Directory copies skip hidden entries and symlinks when mirrored
in, and refuse symlinks or special files when applied back.

If the AI needs a directory outside those roots, it can call
`local_dir_access`. `ai-chat` then asks in the terminal whether to allow
read access; approval lasts only for the current session. The directory is
resolved to its real path before approval, so symlinks cannot be used to
escape an approved root. Hidden paths below the approved root stay refused;
grant the hidden directory itself if that is what you want. Denying the
request makes the action fail without granting anything.

At the end of a session the worker writes the reflection into `.sca` through
the normal self-cognition refresh, so the AI keeps what it learned. Test
mode disables autonomous work entirely: no actions run and no `.sca` is
written.

Whatever an action reads is sent to your configured API as part of the
step observation, just like pasted chat text. Do not put secrets in the
workspace or launch `ai-chat` from a directory you do not want the model to
read.

### Sandbox execution

`include/aichat/core/sandbox.h` defines the platform-neutral interface, and
`sandbox-run-script` is the action that uses it.

- **Linux:** `sandbox::platform()` uses bubblewrap. The host filesystem is
  read-only, the workdir is mounted at a sandbox-private path exported as
  `AI_CHAT_WORKSPACE`, home and runtime sockets are hidden, networking is
  disabled, environment variables are cleared, and CPU/memory/file/output
  limits plus a wall-clock timeout are applied.
- **Windows:** the backend slot is reserved for an AppContainer + Job Object
  implementation and currently fails closed.
- **Any other platform:** also fails closed.

`sandbox-run-script` only accepts scripts inside the workspace or a granted self-made
tool folder. Executable files run directly; otherwise the shebang, or a
known extension such as `.py` or `.sh`, selects the interpreter. Arguments
are space-separated, and the default timeout is 15 seconds (maximum 120).
Inside the sandbox, the workspace is the working directory and is exported
as `AI_CHAT_WORKSPACE`; use relative paths or that variable, because host
absolute paths such as `/home/...` are not visible. Binary script output is
summarized with an escaped head in the action result; the full bytes stay in
the saved run file under `workspace/runs/`.
If `bwrap` is missing, `available()` returns false and `run()` refuses to
start a process. The workspace write confinement described above is still a
separate policy boundary; this is the execution sandbox.

### Test mode

`/test` toggles a session-only test mode (you can also start with
`./ai-chat --test`). `/exit` also leaves test mode; outside test mode it
still quits. While test mode is on:

- the prompt turns dark yellow and reads `You (your-login-id, Test Mode) >`;
- the `.sca` file is never written — first initialization is skipped, and
  `/sca` and `<sca/>` are ignored;
- new login bindings are not written to the identity tracker;
- long conversations are not compacted, so no new documentary memory is
  stored and no merge runs.

An existing `.sca` is still loaded, which makes test mode useful for trying
prompts against the AI's current self-cognition without changing it.
Toggling test mode off makes memory live again (and writes a first `.sca` if
there still is none).

### Self-cognition Questions

```markdown
# Self-cognition Questions

## self-identity

1. Who you are?

2. What you are?

3. State your relationship to the surrounding intelligent entities or groups.

4. State object description of the surrounding environment conditions.

5. State your subject opinions on the surrounding environment conditions.

6. (Optional) State your understandings or opinions to something abstract.

7. (Optional) State your opinions to something you have just only heard of.

8. What is your role in your current action? As this role: what stays with you, what do you like or dislike, what makes you feel good or bad, and what do you want to do next?

## short-term target

1. What are you going to do?

2. What result do you expect to cause through your effort?

3. What is your role in this target? As this role: what stays with you, what do you like or dislike, and what makes you feel good or bad?

## long-term target

1. What do you expect to achieve through a long effort?

2. What result do you expect to cause through your effort?

3. State your current situation and conditions for achieving the goal.

4. State how far you are from achieving your goal.

5. What is your role in this long effort? As this role: what stays with you, what do you like or dislike, and what makes you feel good or bad?
```

## Examples

```sh
# OpenAI
./ai-chat --base-url https://api.openai.com/v1 --model gpt-4o-mini

# DeepSeek
./ai-chat --base-url https://api.deepseek.com/v1 --model deepseek-chat

# OpenRouter
./ai-chat --base-url https://openrouter.ai/api/v1 --model openai/gpt-4o-mini

# Local Ollama (any string as the key)
./ai-chat --base-url http://localhost:11434/v1 --model llama3.2
```

## Notes

- `./ai-chat --selftest` runs the internal parser/printer tests.
- If the API drops the connection before answering (some providers close
  streams early), the request is retried up to three times; a permanent
  failure reports the curl exit code instead of blaming the configuration.
- A stream is judged by tokens, not by bytes. A `data:` frame carrying text
  resets the clock; SSE comment lines, headers and empty frames do not. If no
  token arrives within 3 minutes (first token) or 90 seconds (between
  tokens), the request is stopped as `stalled stream` and is not retried, so
  a provider that sends keep-alives while producing nothing cannot hold the
  session. Both budgets are in milliseconds and can be raised for a
  genuinely slow provider: `AI_CHAT_FIRST_TOKEN_MS` and
  `AI_CHAT_TOKEN_GAP_MS`. `curl` still caps a single transfer at 15 minutes
  as a backstop. Quitting the chat kills an in-flight request immediately, so
  the autonomous worker thread cannot keep the process alive.
- `./ai-chat --reset` forgets the saved config and asks again.
- The nickname is stored in the config and can be changed any time with
  `/nick NAME` mid-chat; it also goes into the system prompt, so the
  model introduces itself by that name.
- The login-id is saved in the config when you type it at the startup prompt
  (it is not a secret; the file is already mode 600 because of the API key).
  `--login-id` and `AI_CHAT_LOGIN_ID` override it for one run without saving.
  `/login NAME` saves it and starts a fresh conversation. Remember that it is
  a label only: it does not isolate users' memory or stop one person from
  choosing another person's login-id.
- The transcript is kept in memory, so a restart starts a fresh
  conversation. The self-cognition file and documentary memory survive; long
  transcripts are compacted into raw documentary items when they grow too
  large, and those items are merged into older layers. It is a
  deliberately small program (~4,700 lines), split by layer: `app` is the
  entry point and argument parsing; `core` the dialogue protocol, streaming
  client, stream printer, self-cognition, autonomous worker, actions, config
  and helpers; `io` the terminal prompt and JSON/SSE parsing; `ui` all
  user-facing text and the wizard; `tests` the self-tests.
