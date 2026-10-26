# Instruction Manual for `<track .../>`

## Syntax

```text
<track id="OBJECT-ID" kind="alias" text="the android book"/>
```

- `id` — an active object id shown in the identity tracker.
- `kind` — `alias`, `role`, `relation`, `fact`, `opinion`, or `note`.
- `text` — one short line. Do not use double quotes or `/>` inside it.

## Effect

The app appends one identity line to an existing object in
`$AI_CHAT_HOME/.config/ai-chat/identity-tracker.mem`. It validates the id,
requires the object to be active, trims the text, rejects duplicates, saves
atomically, and strips the tag from the reply.

The model never reads or writes the file; the app does.

## When to use it

Use it after the user confirms what an ambiguous reference means:

```text
User: that android book
You:  Do you mean [Book] Do Androids Dream of Electric Sheep?
User: yes
You:  Got it. <track id="book-androids" kind="alias" text="the android book"/>
```

Then the alias is available on the next turn, and the book moves into the
detailed part of the tracker view when the user mentions it.

## Rules

- Use the exact object id shown in the tracker or its index.
- Only after the user confirms. Never guess a reference.
- Keep `text` to one short line. Do not use a quote character in it.
- Do not show or mention the tag to the user.
- If the object does not exist yet, ask the user to run
  `/track [Category] Name: identity; ...` instead.
- Test mode disables the write.

## Combining tags

`<track .../>` may appear with `<sca/>` or `<timer N/>` in the same reply. The
app applies the track request first, then rewrites the self-cognition file and
chooses the countdown.
