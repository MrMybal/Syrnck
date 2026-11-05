# Native linear rebase

Native rebase planning replays the explicitly selected linear range `(BASE, HEAD]`
onto another revision. `BASE` is excluded and must be reached by following the
branch's single-parent chain. There is no implicit merge-base selection.

```text
syrnck native rebase-plan OLD_BASE --onto upstream --verify
syrnck native rebase-plan OLD_BASE --onto NEW_BASE --max-commits 500
```

The CLI accepts normal native selectors; C++ takes exact revision IDs. `--branch`
selects the target branch, defaulting to the current native branch. `--directory`
selects its workspace, subject to existing workspace binding checks. The workspace
must have clean tracked files, no obstructing untracked/ignored paths, no staging
index and no pending checkout/switch recovery.

## File replay and topology

Commits replay oldest first with the shared three-way file merger. Each original
parent is the baseline, the previous virtual result is current, and the original
commit is incoming. Independent file/content/mode changes combine. Content,
add/delete and portable namespace conflicts stop planning at the first conflicted
commit. No later commit is treated as applied. Automatic text merge and rename
detection remain future work.

A merge inside the selected range is a `merge_in_range` blocker; a merge at the
excluded boundary is allowed. Reaching a root before BASE gives
`base_not_on_linear_history`. Rebase never guesses a different range or silently
flattens merge parents. The default limit is 1,000 selected commits, configurable
with `--max-commits` in 1..100,000. Exceeding it fails the bounded traversal.

With BASE equal to ONTO, a valid range is already up to date and preserves exact
commit identities. An empty range (HEAD equals BASE) proposes moving the branch
to ONTO. Otherwise every original commit gets one new parent, its preceding
virtual result. Original messages and timestamps are preserved. Logically empty
replays are retained as commits instead of being silently dropped. Representation
changes alone are not content edits, matching cherry-pick's file-level policy.

## Read-only report and bounds

The JSON report includes `head`, `base`, `onto`, `mode`, ordered `commits`, replay
`steps`, `completed_commits`, proposed `target`, workspace preflight, `ready` and
`blockers`. A step contains original `source`/`parent`, its virtual `head`, proposed
`target`, `empty`, file changes and conflicts. A ready plan exits 0; a blocked plan
exits 1 with JSON on stdout. Invalid options or missing required metadata produce
diagnostics. A blocked range or conflicted result has null target/workspace.

Virtual candidate IDs use the ordinary revision object identity algorithm and
complete candidate metadata validation. Planning does not store candidate objects
or move any reference/file. A proposed ID may therefore not be inspectable in the
Store. Each candidate preserves the source message/timestamp and uses the computed
file map and single new parent. Previewing again gives the same IDs for the same
inputs. `--verify` additionally audits complete original-head/base/onto closures
and reconstructed file hashes. Metadata-only planning does not audit block payloads.

Reports and each candidate's flat revision metadata are limited to 16 MiB. The
planner retains a bounded ID list/report and the current decoded snapshots, rather
than every candidate's complete file map. It may read revision metadata more than
once. A large number of per-step changes can reach the report limit before the
commit-count limit. This is not a streaming tree representation.

```cpp
syrnck::NativeRebaseOptions settings;
settings.max_commits = 500;
auto plan = store.plan_rebase(project, "feature", old_base, new_base,
                              settings, true, operation);
```

Planning supports English/French progress/errors and cooperative cancellation;
JSON fields/reason codes remain English. Callbacks must not reenter the Store.
The planning API is exposed through C++ and CLI; C/Go native history bindings,
interactive rebase and preservation of merges remain future work.

## Apply and recover

```text
syrnck native rebase OLD_BASE --onto upstream --expect OLD_TIP_ID
syrnck native checkout-status
syrnck native checkout-recover
syrnck native checkout-recover --abort
```

Apply recomputes and verifies the entire ready plan before storing candidates.
It then repeats the deterministic calculation, storing complete new revisions
oldest first, and checks that the final ID matches the verified preview. It does
not retain every candidate's full snapshot in memory. Each candidate remains an
ordinary immutable v1 revision. Empty commits preserve their source metadata.

Only the final candidate is applied to the workspace through the existing
recoverable checkout journal. Intermediate steps never move the branch or working
files. The old target tip is retained in `archive/checkout/TRANSACTION_ID`, while
the final new chain retains ONTO and every replayed candidate. Original intermediate
revisions and their file blocks therefore remain reconstructible after GC.
Use [the archive browser](NATIVE_ARCHIVES.md) with `--kind checkout` to locate them.
Source branches/tags and symbolic HEAD selection do not move.

`--expect` is an exact old tip ID; otherwise the CLI captures it under the Store
lock. The C++ API requires it explicitly. Up-to-date application returns
`state: unchanged` without writes, but still verifies stored data and workspace
preflight. An empty range checks out the exact ONTO revision without new commits.
Conflicts, staging, dirty tracked files or an incomplete history prevent application.

Cancellation before journal publication preserves branch/workspace and can leave
complete unreferenced candidates. After publication, inspect and explicitly
resume or abort the checkout journal. Abort is allowed before branch publication;
afterwards recovery only completes cleanup. Opening the Store never applies
recovery automatically. Active journals block GC and new history mutations.
Prepared files/backups are plaintext even with encrypted object storage; the
existing [checkout durability/data limits](NATIVE_CHECKOUT.md) apply.

The direct result adds `operation: rebase`, `mode`, `previous_revision`, `base`,
`onto` and `replayed_commits` to the checkout receipt. Recovered journals report
their generic checkout identity and recorded target. Revision/journal formats
are unchanged. Retention roots have no automatic expiry.

```cpp
auto applied = store.rebase_directory(project, "feature", expected_id,
                                       old_base, new_base, settings, operation);
```

Tests compare preview/application IDs, ordered parents and preserved messages/dates,
empty commits/ranges, retained historical bytes after GC, unchanged upstream refs,
wrong expected tips, missing historical blocks, cancellation, reopen/resume and
abort across plain/encrypted and loose/packed Stores.
