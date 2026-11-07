---
id: native-reset
title: Native branch reset
description: Native branch reset guide.
section: Version control
order: 340
parent: native-history
revision: 1
---

# Native branch reset {#native-branch-reset}

Branch reset moves an existing branch to a selected revision while leaving every
working file untouched. It archives the old tip and any previous staging snapshot
in the same atomic reference update. Ordinary `native reset [PATH...]` continues
to unstage files; moving a branch requires an explicit mode.

```text
syrnck native reset-plan REVISION --soft --verify
syrnck native reset REVISION --soft --expect OLD_TIP_ID
syrnck native reset REVISION --mixed --expect OLD_TIP_ID
```

| Mode | Branch | Prepared snapshot | Working files |
| --- | --- | --- | --- |
| `--soft` | Selected target | Preserve file entries and rebase the staging baseline onto the target | Untouched |
| `--mixed` | Selected target | Archive and clear the index | Untouched |

Without an explicit index, soft reset freezes the old tip's complete file entries
into a staging snapshot based on the new target. `native commit --staged` can then
recommit those bytes as a child of the target. Existing prepared bytes are retained,
including changes different from the old tip and later working files. A stale index
blocks soft reset; mixed reset explicitly archives and clears it. An index whose
identity/metadata is invalid is rejected in both modes.

The target can be any valid native revision; ancestry is not required. The CLI
accepts the usual native selectors. `--branch` overrides the selected branch
without changing symbolic HEAD. API methods require exact IDs and an existing
branch. `--expect` guards the exact old branch tip; if omitted, the CLI captures it
under the exclusive Store lock. Staging is captured at invocation and checked in
the same publication, rather than frozen by an earlier plan.

Plans emit JSON and exit 0 when ready, 1 with JSON for blockers. They read metadata
only; `--verify` audits complete head/target/index closures and reconstructed file
hashes. The soft plan includes the proposed staged comparison against the target.
Reports and prepared revision metadata retain the existing 16 MiB limits.
Memory grows with decoded flat snapshots and the bounded staged comparison.
Filesystem working contents are neither inspected
nor validated, so dirty/untracked files remain available for subsequent status.
Pending checkout/switch recovery, conflicting archive roots or a stale soft
index prevent publication. A reset to the same tip is a no-op, except mixed reset
still archives and clears an existing index.

## Retention and interruption {#retention-and-interruption}

The previous tip remains rooted at `archive/reset/OLD_ID`. Any former index remains
rooted at `archive/reset-index/INDEX_ID`, including its baseline history. These are
ordinary revision roots; all their manifests and blocks survive destructive GC
in plaintext/encrypted and loose/packed Stores. Tags and source branches do not
move. The operation verifies all old and new retained closures before publication;
a missing old block prevents even mixed reset from discarding its only root.

The branch, index, history archive and index archive publish through one atomic
`refs.json` replacement. Cancellation or observer failure before publication keeps
the old references. Soft reset preparation can leave an unreferenced immutable
staging revision. A killed publisher leaves the entire old or entire new root set;
inspect `refs`, `native head` and `native staged` before retrying with the old
expected tip. There is no working-file journal to resume because no files change.
No callbacks or cancellation checks run after publication.

Archives have no automatic expiry. Find retained versions using
[the archive browser](doc:native-archives), then export the old ID or create a recovery branch:

```text
syrnck native export OLD_ID recovered-version
syrnck native branch recovered OLD_ID
syrnck native export INDEX_ID recovered-prepared-files
```

Generic reference tools can remove these roots; removing the last root permits GC
to collect exclusive data. Working edits never ingested by commit/add are not
stored by reset. [Recoverable hard reset](doc:native-hard-reset) also captures dirty working bytes. Automatic restoration of a former branch/index pair remains separate work. Native revision v1 is unchanged.

```cpp
auto plan = store.plan_reset_branch("main", target_id, syrnck::NativeResetMode::soft,
                                    true, operation);
auto result = store.reset_branch("main", expected_id, target_id,
                                 syrnck::NativeResetMode::soft, operation);
```

C++ and CLI expose these operations; native history C/Go bindings remain future
work. Progress/errors support English and French; JSON field/reason names remain
English. Callbacks must not reenter the Store.
