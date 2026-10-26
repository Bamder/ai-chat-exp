# Instruction Manual for `<timer N/>`

## Syntax

```text
<timer N/>
```

`N` is a whole number of seconds. Spaces around `N` and an optional `=` are
tolerated, but the normal form is `<timer 90/>`.

## Effect

The app uses `N` as the countdown for the user's next turn. The countdown
pauses while the user is typing or editing and resumes after one second of
inactivity.

The tag is removed from the displayed and stored reply.

## Choosing N on your own initiative

Use a value between 30 and 180 seconds. Values outside that range are clamped.
Pick something shorter for a quick yes/no and longer for a question that needs
thought.

If you set no timer, the configured default from the prompt is used.

## Exact times from the user

If the user gives an exact duration or clock time, calculate the number of
seconds yourself and put that exact value in the tag. This may exceed 180
seconds, up to the app's hard limit of 24 hours.

Examples:

- "wait 5 minutes" -> `<timer 300/>`
- "I'll be back in 10 minutes" -> `<timer 600/>`
- "wait until 15:30" -> calculate from the current local time in the prompt.

Only do this when the user actually supplied the time.

## When not to use it

- Do not use `<timer N/>` together with `<wait/>`; waiting has no countdown.
- Do not use it when no countdown is active.
- Do not use a timer to pressure the user after they asked for unlimited time.
- Do not show or mention the tag to the user.
