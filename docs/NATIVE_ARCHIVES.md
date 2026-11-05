# Native history archives

Native history mutations retain former revisions and prepared bytes as ordinary
GC roots. The archive browser lists those roots without changing references,
objects or working files. It also covers existing amendment, checkout and history
detachment archives.

```text
syrnck native archives --limit 100
syrnck native archives --kind reset-index --verify
syrnck native archives --after archive/reset/OLD_ID --limit 100
syrnck native archive-show archive/squash/OLD_ID --verify
```

| Kind | Reference | Retained data |
| --- | --- | --- |
| `reset` | `archive/reset/OLD_ID` | Former branch tip and its full history |
| `reset-index` | `archive/reset-index/INDEX_ID` | Former prepared snapshot and its baseline history |
| `squash` | `archive/squash/OLD_ID` | Original chain, including intermediate file versions |
| `amend` | `archive/amend/OLD_ID` | Former amended tip and its parents |
| `checkout` | `archive/checkout/TRANSACTION_ID` | Source revision retained by completed checkout |
| `detached` | `archive/OLD_ID` | Original tip retained by history detachment |

Tag deletion records use the existing `native tag-archive` / `tag-restore`
workflow and are excluded. Custom/unknown archive namespaces are excluded; `refs`
remains the generic complete reference inventory. The browser checks owned archive
names, revision target kinds and ID identity. Checkout names identify a transaction
rather than the target revision. A manually corrupted owned archive is an error.

## Pages and verification

`--kind` optionally selects one table category. `--limit` is 1..1000, default 100.
Rows are ordered by full reference name. `next_after` is the exclusive cursor for
the next page when `has_more` is true, otherwise null. A cursor remains usable if
its reference has since been removed, but its category must match an explicit
filter. Pages reflect the current reference table on each invocation; they are
not a persistent snapshot across mutations.

Each row contains `reference`, `kind`, exact `revision` ID, file/parent counts,
the revision's timestamp and a message preview of at most 256 UTF-8 bytes without
splitting a code point. `message_bytes` and `message_truncated` describe the full
message. The timestamp belongs to the stored revision, not the archive event.
`native archive-show` returns the same row for one full reference; use `inspect
revision ID` for complete revision metadata.

Default queries read only reference/revision metadata. They can list a revision
whose blocks are missing. `--verify` additionally audits each returned revision's
complete closure and reconstructed file hashes; missing/corrupt retained bytes
fail the query. List verification covers the returned page, not undisplayed
archives. Progress/cancellation and English/French diagnostics are supported;
JSON fields remain English. Callbacks must not reenter the Store.

The compact report is bounded to 16 MiB. The underlying reference table is still
loaded in full; pagination bounds returned rows and revision metadata reads,
rather than reference-table memory. Plain/encrypted and loose/packed Stores use
the same logical roots. No format or retention policy changes are introduced by
inspection.

## Recovering stored versions

Use the row's `revision` ID to export files or create a recovery branch:

```text
syrnck native export REVISION_ID recovered-version
syrnck native branch recovered REVISION_ID
syrnck native switch-plan recovered --verify
syrnck native switch recovered
```

The existing checkout/switch clean-workspace and recovery rules apply. A prepared
index archive can be exported in the same way to recover its frozen bytes; the
browser does not automatically restore a branch/index pair or alter working edits.
Working files never imported into the Store are outside archive retention.

Archives do not expire automatically. Generic reference removal can drop a root;
when its last retaining root disappears, exclusive objects become eligible for
destructive GC. The browser neither prunes archives nor promises disk reclamation.

```cpp
auto page = store.native_archives("reset-index", {}, 100, true, operation);
auto archive = store.native_archive("archive/reset/" + old_id, true, operation);
```

Available through C++ and CLI. Native history C/Go bindings remain future work.
Tests cover every category, filtered pagination, cursor validation, foreign/tag
exclusion, UTF-8 truncation, missing blocks, cancellation, read-only inspection
and reconstruction after GC across encryption and pack variants.
