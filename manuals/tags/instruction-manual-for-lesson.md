# Instruction Manual for `<lesson>`

## Syntax

```text
<lesson>PURPOSE: ...
PRACTICE: ...
EVIDENCE: ...</lesson>
```

The body is the lesson itself. It may contain line breaks. Do not put a file
path, directory, or shell command in the tag.

## Effect

After the reply, the app appends the body to the current target's practice
file, `.exp`, through the experience layer. The lesson is also added to the
cross-target practice digest so other targets can see that the rule exists.

The target is chosen by the app:

- the current autonomous target, if a loop is active or paused;
- otherwise the last autonomous target seen in this session;
- otherwise the stable `chat` target.

You never choose or write the path. The app validates the write and keeps the
practice file.

## When to use it

Use `<lesson>` when you have learned something reusable that should survive
the conversation — a rule about how to work, not a detail about the current
task. Write it as:

- `PURPOSE`: the problem the lesson prevents;
- `PRACTICE`: the action to take next time;
- `EVIDENCE`: the concrete moment that taught it.

Keep it short. One tag holds one lesson. If you have two unrelated rules, use
two tags.

## When not to use it

- Do not use it for a diary entry, a feeling, or a scene reaction; those
  belong in the journal, not in practice.
- Do not use it to restate the current target.
- Do not use it as a substitute for actually doing the work.
- Do not use it in test mode; experience writes are disabled there.
- Do not add attributes or a path: `<lesson target="...">` is not supported.

## Relationship to the autonomous loop

Inside the autonomous loop, prefer the `lesson(text)` action, which writes to
the same `.exp` file with the current target already known. `<lesson>` is the
chat-side channel for the same idea: a small, app-owned way to keep a rule
without opening the file system or starting a whole worker.
