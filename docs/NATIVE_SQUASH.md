# Native commit squash

Squash replaces the last 2..100,000 linear commits with one commit containing the
exact former tip's file entries. It changes neither prepared nor working files.
The complete original history remains retained in an archive.

```text
syrnck native squash-plan 3 --verify
syrnck native squash 3 -m "Implement asset streaming" --expect OLD_TIP_ID
```

The range includes the current tip and follows its single-parent chain. The new
commit's parent is the parent of the oldest included commit, or no parent when
the range reaches the root. All manifests, executable modes and representation
choices come from the old tip; no files are reconstructed or reingested to build
the candidate. The new message is required and the timestamp is current UTC.
The complete candidate remains subject to the ordinary 16 MiB revision limit.

A merge inside the selected range is a `merge_in_range` blocker. Squash does not
silently select a merge parent. A merge immediately before the range is preserved
as the new parent, including its existing ancestry. Requesting more commits than
exist produces `count_exceeds_history`. Any staging snapshot, including a clean
one, blocks the operation, as do pending checkout recovery and conflicting archive
roots. Clear/commit staging explicitly before squash.

Plans are read-only. They report the old `head`, requested `count`, visited
`revisions` from newest to oldest, proposed `parents`, file count, archive reference,
`ready` and blockers. Incomplete ranges have null `parents`. A ready plan exits 0;
a blocked plan exits 1 with its JSON on stdout. Invalid counts/metadata instead
produce diagnostics. The compact report is limited to 16 MiB. Memory grows with
the bounded revision-ID list and currently decoded flat revision metadata.

`--verify` audits the old head's complete retained closure and reconstructed file
hashes. Apply always verifies it before preparing any candidate, then verifies
replacement/archive roots before publication. Metadata-only preview does not
promise that historical block payloads are available. Missing ancestor bytes
block publication even if the new tip snapshot no longer uses them.

## Publication and retained history

The branch moves to the new revision and `archive/squash/OLD_HEAD_ID` retains the
entire original chain in one atomic reference update. The expected absent staging
root is checked in that transaction too. Tags, other branches and symbolic HEAD
selection do not change. Working files may contain uncommitted edits; the engine
does not inspect them or include them in the candidate.

`--branch` overrides the selected branch. `--expect` requires an exact old revision
ID; if omitted, the CLI captures the current tip under the exclusive Store lock.
The C++ API always requires that expected ID. A process interruption leaves the
complete old or new reference set. Before publication, cancellation or callback
failure preserves the old references; an immutable unpublished candidate may
remain. There is no working-file journal. Inspect `native head` and `refs` before
retrying with the original expected tip. No callback or stop check runs after
reference publication.

The [archive browser](NATIVE_ARCHIVES.md) locates the original heads and prepared
versions. Archives have no automatic expiry and retain every intermediate file version
through destructive GC in plaintext/encrypted and loose/packed Stores. To recover
a former version, export its ID or create a branch at it. Removing the last root
with generic reference tools permits GC to collect its exclusive data. Squash
shortens visible branch history; it does not reclaim archived history automatically.
Revision v1 is unchanged. Author/signature metadata is not supported by that format.

```cpp
auto preview = store.plan_squash("main", 3, true, operation);
auto applied = store.squash_commits("main", expected_id, 3,
                                    "Implement asset streaming", operation);
```

English/French progress, cooperative cancellation and the usual callback
non-reentrancy contract apply. Exposure is through C++ and CLI; native history
C/Go bindings remain future work. Tests cover root/partial ranges, merge boundaries,
exact tip entries, dirty working files, indexes, expected tips, cancellation,
damaged ancestor blocks and reconstruction after GC. Process-kill cases cover
partial/root ranges through candidate writes and atomic archive publication,
with dirty working files preserved in plain/encrypted Stores.
