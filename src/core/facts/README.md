# Fact engine

The autonomous loop needs one trustworthy answer to "what is true right now?".
This directory is that answer. It replaces the earlier untyped key/value
ledger with typed facts and an explicit verification step.

## Layers

```
action::Result (with reported facts)
      │
      ▼
 fallback()         fallback.cpp   only when the action reported none:
                                   Failure / Generic / nothing for think
      │
      ▼
 verify()           verifier.cpp    stat / waitpid / action-state checks
      │
      ▼
 facts::Store       store.cpp      one state record, dedupe, supersede, persist
      │
      ▼
 render()           store.cpp      compact blocks for planner, checker, observer,
                                    reflection, and the .sca refresh
```

The layers are one-way. Actions own their typed facts; `fallback()` only
fills gaps and never parses output; `verify()` only checks; `Store` only
records. Nothing here decides what to do next; the loop guards
(`src/core/guards.*`) are a separate, thin consumer of the same facts. They
are not a controller: they never choose or rewrite a plan.

## Guards (not a controller)

`guards` enforces only invariants that verified facts cannot enforce by
themselves:

- `RepeatGuard` — the same action and arguments with no new fact is refused;
  a second refusal in a row stops the loop as stalled.
- `checkDone` — accepts a done claim when the model names evidence in
  `<done evidence="key1, key2"/>` and every named fact exists and is
  `Verified` (a `run:` fact must also have `exit=0`). A bare `<done/>` falls
  back to the generic rule: open-ended targets, targets that never used a
  tool, or targets with verified output are allowed; otherwise refused.
- `checkGiveUp` — giving up is refused once while a verified artifact exists,
  so the model has to inspect what it already produced.

Everything else — what to try, when to backtrack, how to phrase the plan —
stays with the model.

## Files

| file | role |
|---|---|
| `fact.h` | `Fact`, `FactKind`, `FactStatus`, `make()` |
| `text.h/.cpp` | compacting, field extraction, persistence escaping |
| `fallback.h/.cpp` | gap filler: Failure/Generic when an action reports no facts |
| `verifier.h/.cpp` | claims → verified / contradicted |
| `store.h/.cpp` | eventless state: merge, supersede, render, save |

## Fact kinds

- `FsFile`, `FsDir` — a path exists with the expected type.
- `Listing` — the entries of a directory.
- `Wrote` — a write or mirror-apply landed.
- `Run` — exit code, timeout, truncation, output size.
- `Grant` — read access active for the session.
- `Tool` — a self-made tool folder exists.
- `Mirror` — a copied artifact and its origin.
- `Rejection` — a planned step was refused before running.
- `Failure` — an action failed.
- `Generic` — anything not yet classified.

Each fact carries `status`:

| status | meaning |
|---|---|
| `Verified` | the app checked it (`stat`, `waitpid`, grant state) |
| `Reported` | taken from action output, not independently checked |
| `Contradicted` | the app checked and the claim is false |
| `Superseded` | replaced by a newer state fact with the same key |

## Adding a new action

1. Add the action. If it reports no facts it still gets a `Failure` or
   `Generic` fact from `fallback()`, so it is never fact-less.
2. Report its facts from the action itself with
   `r.facts.push_back(facts::make(...))`. Use the resolved path, not the
   output text. `read_file`, `list_dir`, `write_file`, `append_file`,
   `sandbox-run-script`, `local_dir_access`, `self-made-tool` and both mirror
   actions do this today.
3. If the claim can be checked independently, teach `verify()` how. Prefer a
   cheap check (`stat`, exit status, session state) over trusting output.
4. Do not add a parsing rule anywhere. `fallback()` is deliberately free of
   action knowledge.

## Rendering

`Store::render()` writes, in decision-relevance order:

1. verified facts
2. contradictions
3. failures and rejections
4. reported facts
5. superseded facts

The render is capped so a long session cannot flood the model's context. The
raw action output remains available in the journal; the fact store is the
authoritative summary.

## Persistence

`$AI_CHAT_HOME/.config/ai-chat/autonomy-facts.mem`, tab-separated, escaped,
atomic write. It is keyed by the short-term target: the same target resumes; a
different target starts fresh. Before the reset, the worker writes the previous
target's current facts to `ai-experiences/<target>.exp` as a `task close-out`,
so the knowledge is archived rather than dropped. When `kMaxFacts` or
`kMaxSuperseded` evicts a fact, the store buffers it in `takeEvicted()` and the
worker writes a `facts evicted` line to the same experience log. If that write
fails, the record is persisted in `pending-experience.mem` and retried on the
next worker cycle or session. Durable world facts do not live here; they are
kept by the separate identity tracker.
