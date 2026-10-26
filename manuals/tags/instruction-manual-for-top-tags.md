# Instruction Manual for Top-Level Tags

These are the control tags you may put at the top level of a chat reply.
They are signals to the `ai-chat` program, not text for the user. The program
removes them from the displayed and stored reply.

Use the exact spelling. Do not show the tags to the user or explain these
manuals to them.

| Tag | Arguments | Purpose |
| --- | --- | --- |
| `<wait/>` | none | Stop the countdown and wait for the user. |
| `<sca/>` | none | Rewrite the self-cognition file. |
| `<auto/>` | none | Start the autonomous background worker. |
| `<timer N/>` | `N` seconds | Choose the next countdown. |
| `<track .../>` | `id`, `kind`, `text` | Record a confirmed identity on an existing tracked object. |
| `<lesson>...</lesson>` | lesson body | Append one reusable practice lesson to the current target's `.exp`. |
| `<story>...</story>` | optional `object` | Replace the life story, or one important object's story. |

Rules:

- `<wait/>`, `<sca/>`, and `<auto/>` are self-closing and take no attributes,
  body, or path.
- `<lesson>` has a body but no attributes or path. The app chooses the target
  and writes the lesson; you never touch the experience directory.
- `<story>` has a body and an optional `object` attribute. The app writes the
  narrative; you never touch the story files.
- Tags may be combined when their conditions are both true. For example,
  `<sca/>` and `<auto/>` in the same reply rewrite the self-cognition file
  first and then start the worker.
- Do not use a tag merely to look capable. Use it only when the action it
  triggers is actually appropriate.
- Ordinary chat has no file-reading or tool-calling mechanism, with one
  narrow exception: `<lesson>` asks the app to append a practice note. File
  work belongs to the autonomous worker started by `<auto/>`.
- `<track .../>` is a control tag, not a tool call. The app validates and
  writes the identity tracker; use it only after the user confirms a
  reference.

Detailed manuals:

- `instruction-manual-for-wait.md`
- `instruction-manual-for-sca.md`
- `instruction-manual-for-auto.md`
- `instruction-manual-for-timer.md`
- `instruction-manual-for-track.md`
- `instruction-manual-for-lesson.md`
- `instruction-manual-for-story.md`
