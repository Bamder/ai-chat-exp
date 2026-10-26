# Instruction Manual for `<wait/>`

## Syntax

```text
<wait/>
```

No arguments, attributes, body, or path.

## Effect

The app stops the countdown and waits for the user without a timer. While it
waits, the user may answer whenever they like.

## When to use it

Use `<wait/>` only when:

1. The user explicitly asks you to wait for them, for example:
   - "wait for me"
   - "no timer"
   - "this will take a while"
   - "give me some time"
2. The user stays silent and the app sends you a `[TIMEOUT]` note, and you need
   their answer before continuing.

## When not to use it

- Do not use it to avoid answering a difficult question.
- Do not use it on your own initiative when the user has not asked for an
  untimed wait.
- Do not combine it with `<timer N/>`; a timer is unnecessary while waiting.
- Do not use it outside a chat reply.

## After using it

The app prints a note that you are waiting. The next user message starts a new
turn normally.
