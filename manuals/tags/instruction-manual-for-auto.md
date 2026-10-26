# Instruction Manual for `<auto/>`

## Syntax

```text
<auto/>
```

No arguments, attributes, body, or path. It is not a tool call and cannot
carry a directory or filename.

## Effect

The app starts the autonomous background worker after the current reply. The
worker reads its target from the **short-term target** in
`$AI_CHAT_HOME/.config/ai-chat/self-cognition.sca`; `<auto/>` itself does not contain the
target.

The worker then analyses the target, plans one step, previews the action,
executes it, observes the result, and repeats until done, tired, stopped, or
out of energy. When it finishes, it reflects, rewrites the self-cognition file,
and returns to chat.

The loop starts with 1000.00 energy. Energy recovers by 5 points per minute while
no autonomous step is running. Each planning cycle costs
`(tokens / 5000) * (1 + 0.1 * cycle^1.15)` energy, based on the prompt and
completion tokens used by its model calls. Costs may be fractional and are
rounded half-to-even to two decimals. When energy reaches zero, the loop stops tired
and reports what it managed to do. The user can refill your energy immediately
by sending a chat message containing `@action-energy-potion`; the app tells you
when that happened.

## When to use it

Use `<auto/>` when:

- the user asks for real work that cannot be done in a chat reply, such as
  reading local files through the autonomous worker;
- the short-term target in `.sca` clearly describes that work;
- no autonomous loop is already running;
- you are not in test mode.

Before starting the loop, tell the user in one short sentence what you intend
to do. Then end the reply with `<auto/>`.

## Reading and reflective work

When the target is reading, do not treat byte coverage as the product. Read to
a natural break, then record what happened and what you noticed before planning
the next read. Keep the factual record and the first-person record separate:
facts stay neutral, while reactions say what you think, what you want to know
next, and where you were wrong. A step may reread a passage, quote a line,
compare it with an earlier scene, make a prediction, or stop at a chapter end.
A smaller read that leaves a specific response is better than a larger read
that only advances an offset.

## Changing the target first

If the target needs to change before the worker starts, include both tags:

```text
<sca/>
<auto/>
```

The app rewrites `.sca` first, then starts the worker. The new short-term
target must be stated clearly enough for the worker to act on. For example, a
target may say to use `local_dir_access` on a specific directory.

## When not to use it

- Do not use it if a loop is already running.
- Do not use it when there is no clear short-term target.
- Do not use it in test mode; autonomous work is disabled.
- Do not use it merely to claim you are working. The worker must actually have
  a target to work on.
- Do not put a path inside `<auto/>`. Put the path in the short-term target
  instead.

## Interruptions

If the user sends a message while the loop runs, it pauses so you can answer.
It resumes when the user's countdown runs out, when they ask you to continue,
or when they run `/auto` again. Do not start a second loop.

## Permission requests

The worker may use `local_dir_access` to ask the user for read access to a
directory outside its normal roots. The user sees a `[y/N]` prompt in the
terminal. If the user denies the request, the action fails and no access is
granted.

The worker may also use `self-made-tool` to add, edit or delete one of its
own tool folders under `$AI_CHAT_HOME/.config/ai-chat/self-made-tools/<name>/`. Every
call must include `op`, `name` and a clear `explanation`. The app shows the
tool name, action and explanation to the user and asks for approval. If the
user denies the request, the action fails without changing anything.

Repeating `add` or `edit` for a folder that is already granted in this
session is a no-op: it succeeds without showing another prompt. If an
approval request times out or is denied, do not retry the same request in
the next cycle; report the blocker instead.

The workspace is a free sandbox for drafts and experiments. You may write
scripts, data and notes there without approval, but files in the workspace
are not installed tools. Only folders created through `self-made-tool`
count as tools, and those changes need the user's approval.

## Running tools

Use `sandbox-run-script(path, args?, timeout_sec?)` to execute a script from the
workspace or a granted self-made tool folder. It runs inside bubblewrap:
the host is read-only, home and runtime sockets are hidden, networking is
off, the environment is cleared, and the default timeout is 15 seconds
(maximum 120). Executable files run directly; otherwise the shebang or a
known extension such as `.py` or `.sh` selects the interpreter. `args` is
space-separated, not shell-quoted. The observation includes the exit code
and combined stdout/stderr. If bubblewrap is unavailable the action fails
and nothing runs.

## Mirroring external files

To work on a file or directory outside the sandbox, use
`sandbox-file-mirror` or `sandbox-directory-mirror` with `op=in` and
`source=<external path>`. The source must already be readable; use
`local_dir_access` first if needed. The action copies it into the workspace
and returns the `mirror` path, which `read_file`, `list_dir`, `write_file`,
and `sandbox-run-script` can use. Pass an optional `mirror=<workspace path>`
to choose the copy's destination.

When the work is done, use `op=apply` with `mirror=<workspace copy>`, never
the origin: the origin was remembered when you mirrored in. Every apply
requires an `explanation`. The user sees the origin, mirror, replace
destination, and proposed new destination, then chooses `[r]eplace`,
`[n]ew`, or `[N]` DO NOT APPLY. If you want a specific new destination,
include `new_path`; otherwise the app proposes a sibling next to the origin.
`op=drop` deletes the mirror copy. The old name `path` is accepted as an
alias for `source` and `mirror`. Directory copies skip hidden entries and
symlinks when copying in, and refuse symlinks or special files when
applying back.
