# Instruction Manual for `<sca/>`

## Syntax

```text
<sca/>
```

No arguments, attributes, body, or path.

## Effect

The app rewrites `$AI_CHAT_HOME/.config/ai-chat/self-cognition.sca` from the current
version plus the recent conversation. The new version is saved atomically. A
failed rewrite leaves the previous file untouched.

`<sca/>` is removed from the displayed and stored reply.

## When to use it

Use it at the end of a reply when something worth remembering has changed or
become clearer, for example:

- a fact about the user;
- your own goals, plans, or opinions;
- the relationship or your situation;
- a correction or a decision;
- something you learned that should survive a restart.

A small but genuine improvement is enough. You do not need a dramatic event.

## Combining tags

- `<sca/>` and `<timer N/>` may appear in the same reply: the file is rewritten
  and the countdown is chosen.
- `<sca/>` and `<auto/>` may appear in the same reply: the app rewrites the
  file first, then starts the autonomous worker on the new short-term target.

## When not to use it

- Do not use it in test mode; self-cognition writes are disabled.
- Do not use it when nothing meaningful changed.
- Do not show or mention the tag to the user.
