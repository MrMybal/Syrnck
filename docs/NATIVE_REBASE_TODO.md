# Editable and interactive native rebase

Native linear rebase supports a complete reviewed sequence, either from an
editable JSON document or a terminal prompt. Both routes use the same C++
planner, conflict resolutions, text composition and recoverable checkout.

```text
syrnck native rebase-todo OLD_BASE --onto upstream > todo.json
syrnck native rebase-plan OLD_BASE --onto upstream --todo todo.json --verify
syrnck native rebase OLD_BASE --onto upstream --todo todo.json --expect OLD_HEAD
```

Keep the document outside tracked workspace files (for example, under the
Store) so it does not obstruct checkout. The template contains exact `head`,
`base` and `onto` IDs and one step per source commit. Move rows to change order;
retain every source exactly once and use `drop` explicitly to remove one.
Unknown/missing fields, duplicate JSON keys/sources, foreign commits, stale
contexts and incomplete ranges are rejected before creating candidates.

```json
{
  "version": 1,
  "type": "native-rebase-todo",
  "head": "HEAD_ID",
  "base": "BASE_ID",
  "onto": "ONTO_ID",
  "steps": [
    {"source": "SECOND_ID", "action": "reword", "message": "New message"},
    {"source": "FIRST_ID", "action": "squash"},
    {"source": "THIRD_ID", "action": "fixup"},
    {"source": "FOURTH_ID", "action": "drop"}
  ]
}
```

Replace the illustrative IDs with the 64 lowercase hexadecimal IDs emitted
by the template. Messages may contain Unicode, newlines and empty text.

| Action | Result |
| --- | --- |
| `pick` | Replay the source delta and preserve its message/timestamp. |
| `drop` | Replay nothing; retain its original history in the checkout archive. |
| `reword` | Replay the delta, keep its timestamp and use the required `message`. |
| `squash` | Replay the delta into the preceding picked result, retaining that result's timestamp/parent and appending the source message after two newlines. |
| `fixup` | Replay the delta into the preceding picked result, retaining its message, timestamp and parent. |

Only `reword` accepts a `message`. Squash/fixup require a preceding picked
result, ignoring intervening drops. A fold replaces the previous virtual
candidate rather than introducing an extra parent/commit. Provisional folded
objects are not stored. Original source versions remain protected by the old
head archive; an unpublished provisional snapshot is not a retained version.

Each delta is still measured against the source commit's **actual original
parent**, even after reordering or dropping earlier commits. Dropping a
dependency can therefore cause a conflict. It does not silently apply the
dependent source's complete snapshot. Existing `--resolutions JSON_FILE` and
`--text-merge` can resolve those replay conflicts. Resolution contexts include
the current virtual head, so changing earlier actions invalidates stale choices.
A dropped source cannot have file resolutions.

An explicit sequence is honored even when BASE equals ONTO. Dropping all rows
moves the branch to ONTO through the normal checkout journal. Without a todo,
the original same-base no-op and oldest-first behavior remain available.

## Terminal input

```text
syrnck native rebase OLD_BASE --onto upstream --interactive
```

The CLI prints source IDs and escaped message previews on stderr. Enter the
complete desired sequence, one action and **full** revision ID per line:

```text
reword SECOND_ID New message
squash FIRST_ID
fixup THIRD_ID
drop FOURTH_ID
.
```

A line containing `.` confirms the sequence; `.` alone selects the original
pick sequence. EOF before confirmation fails without writing candidates or
changing files/references. Ctrl+C requests cancellation. Commands are not
executed as shell text. Action names remain English in both interface languages.
Use a JSON document for multiline messages. `--todo` and `--interactive` are
mutually exclusive; interactive prompts cannot be combined with JSON progress.
`rebase-plan --interactive` reviews without applying. Plain result JSON stays
on stdout, separate from the prompts. Redirected input uses UTF-8. Windows
console input is read as UTF-16 and converted without changing the console's
code page or interpreting message backslashes as path separators.

## Limits, recovery and scope

The ordinary linear-range, clean-workspace and complete-file verification
requirements apply. Todo JSON and aggregate interactive input are limited to
16 MiB, with at most 100,000 rows and 1 MiB per explicit/combined new message.
Rebase reports remain independently bounded at 16 MiB and now include `todo`,
per-step `action`, fold `replaces`, `result_commits` and `dropped_commits`.
`completed_commits`/`replayed_commits` count non-dropped source deltas, while
`result_commits` counts the resulting chain after folds. The template is also
available in a normal rebase plan; a non-linear range has no todo template.

Application verifies all inputs, materializes surviving commits and uses the
existing journal to install the final files, publish the branch and archive
the old head. Recover or abort an interrupted checkout with the documented
[rebase recovery commands](NATIVE_REBASE.md#apply-and-recover). A failed or
cancelled preparation can leave unreferenced valid objects for later GC.

This is interactive **linear** rewriting. Merge-preserving rebase, editor
launching, split/edit stops, automatic fixup matching and execution of test
commands are not implemented by these actions.

Tests cover strict documents, reordered deltas, dependent drops/conflicts,
fold metadata, exact virtual IDs, same-base/all-drop rewrites, six storage and
encryption layouts, historical reconstruction after GC, English/French CLI
prompts and refusal to publish unconfirmed input. The separate hard-termination
campaign covers folded candidate publication and checkout recovery/abort.
