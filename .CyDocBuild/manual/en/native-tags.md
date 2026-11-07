---
id: native-tags
title: Native tags, revision selectors and retained archives
description: Native tags, revision selectors and retained archives guide.
section: Version control
order: 400
parent: native-history
revision: 1
---

# Native tags, revision selectors and retained archives {#native-tags-revision-selectors-and-retained-archives}

Native tags name committed revisions independently of a branch. A lightweight
tag retains its target; supplying a message also stores an annotation with the
original UTF-8 text and creation time. These operations change references and
metadata, without reading or changing working files, staging or HEAD.

```text
syrnck native tag release/v1
syrnck native tag release/v2 HEAD -m "Release notes"
syrnck native tags --limit 100
syrnck native tag-show release/v1 --verify
syrnck native diff tags/release/v1 HEAD
syrnck native checkout-plan tags/release/v1 --verify
```

The default target for creation is the current committed HEAD. Local working
edits and staged bytes are not included. An unborn HEAD requires an explicit
existing revision. Creation verifies the target's entire closure, including
parents and complete file hashes, before publication. Existing names are refused,
even when the requested target is identical. There is no force/move operation.

Names follow native reference syntax: ASCII letters, digits, slash, dot, hyphen
and underscore, with no empty, `.` or `..` components. Names cannot start with
`-` and must fit both reference namespaces below (at most 187 bytes). Messages
accept up to 65,536 UTF-8 bytes; an explicit empty message creates an annotated
tag. Operating-system command-line limits can impose a smaller CLI bound.
Annotations are unsigned metadata; tagger identities and signatures remain
future work.

## Read-only reports and pagination {#read-only-reports-and-pagination}

`native tags [--after NAME] [--limit N]` returns `version`, sorted `tags`,
`has_more` and `next_after`. The default limit is 100; the allowed range is
1..1,000. Each tag contains `name`, `revision`, `annotation` (chunk ID or null),
`message` and `timestamp`. The last two are null for lightweight tags. Times are
UTC Unix seconds. JSON keys and values owned by the engine remain English in
both interface languages; user-authored messages are preserved.

Pages are additionally bounded below 16 MiB of serialized JSON, including
escaping. A page can therefore contain fewer tags than its requested limit.
Use the returned `next_after` as the next request's exclusive cursor. The cursor
is a name, so deleting that tag does not invalidate pagination. Each call uses
one reference snapshot; insertions/deletions between calls can change the global
view. There is no persistent listing snapshot or total-count scan.

The page bound is not a process-memory limit. Reference maps, decoded-object
caches, the current annotation and serialization buffers consume additional
memory. Listing validates annotations in the current page and checks for orphan
annotation roots. It avoids loading every revision tree or file payload.

`native tag-show NAME` returns `version`, `tag` and `objects_verified`.
`native tag-archive ID` returns the archived tag plus its `archive` ID and
`deleted_at` time. Both accept `--verify` to audit the full target closure.
Default reports inspect retained references and annotation/record contents;
they do not prove file chunks or historical revisions are available. Missing or
invalid annotations, mismatched target IDs and incomplete archive root sets are
refused. Inspection does not repair or write Store data.

`native tag-archives [--after ID] [--limit N] [--verify]` lists deleted tags in
archive-ID order. It returns `version`, `archives`, `has_more`, `next_after` and
`objects_verified`. Each row contains `archive`, `tag` and `deleted_at`. Limits
are 1..1,000 (default 100), with the same 16 MiB escaped-JSON bound. The exclusive
ID cursor remains usable if that archive is later retired. Every page uses one
reference snapshot and validates complete archive groups and their annotations.
`--verify` audits the complete closures of the displayed groups only; default
listing avoids revision trees and file payloads. It does not create an inventory
snapshot across requests.

## Archived deletion and restoration {#archived-deletion-and-restoration}

```text
syrnck native tag-show release/v1
syrnck native tag-delete release/v1 --expect EXACT_REVISION_ID
syrnck native tag-archives --verify
syrnck native tag-archive ARCHIVE_ID --verify
syrnck gc --delete
syrnck native tag-restore ARCHIVE_ID
```

Deletion requires the exact expected revision ID, as returned by inspection.
`--expect` accepts an ID rather than a moving branch/tag selector. One atomic
reference transaction removes the public tag and annotation roots and publishes
an archive record, target root and optional annotation root. The response contains
the archive ID. If verification or the expected-target check fails, no tag is
removed. Archives retain complete target history and shared chunks during GC,
even after all branch and public tag roots disappear.

If interruption loses the deletion response, inspect the public tag and
`native tag-archives`, then use `native tag-archive ID` to identify the original
name and target. An archived deletion has `archive/tags/ID/record` entries.

Restoration verifies the archive and recreates its original name, target,
message and annotation time in one reference transaction. It refuses a name
already occupied by a tag or annotation root. The archive itself remains
retained. Tags can be deleted and recreated at another revision deliberately;
the deleted version remains available through its archive.

Explicit [backup-protected retirement](doc:archive-retention) accepts
`archive/tags/ID` and removes its complete root group atomically. The verified
independent backup retains its original recipe and payloads for later restoration.
Automatic archive pruning is not implemented. Generic reference/admin APIs can
remove these roots, so API-level immutability is not an authorization or signature
policy. Removing every protecting root and running destructive GC intentionally
makes that version unavailable. Retain all archive roots when preserving or
transferring an archive; the record chunk alone does not retain its target.

## Native revision selectors {#native-revision-selectors}

Native tag creation, branch creation, `status --revision`, snapshot diff,
checkout preflight/execution and revision export accept these selectors:

| Selector | Resolution |
| --- | --- |
| 64 lowercase hexadecimal characters | Exact revision ID, with priority over identically named labels |
| `HEAD` | Current symbolic branch's committed revision; unborn HEAD is refused |
| `heads/NAME` | Explicit native branch |
| `tags/NAME` | Explicit native tag, with annotation consistency checks |
| `NAME` | A branch or tag, provided only one namespace contains that name |

A bare name shared by a branch and tag is ambiguous even if both point to the
same revision. Use its explicit namespace. A tag named `HEAD`, or one named with
a 64-character hexadecimal string, remains addressable with `tags/NAME`.
Resolution validates the selected revision metadata, without traversing its
parents, manifests or file chunks. It is separate from full verification.

`native switch NAME` still selects a literal branch. `native checkout TAG`
restores that revision and moves the selected branch, archiving its old head;
it does not create detached HEAD state. The explicit C++ snapshot methods retain
their revision-ID arguments; applications can call `resolve_native_revision()`.

## Storage, cancellation and API {#storage-cancellation-and-api}

| Reference | Value |
| --- | --- |
| `tags/NAME` | Target revision |
| `tag-metadata/NAME` | Optional canonical annotation chunk |
| `archive/tags/ID/record` | Canonical deletion record chunk, whose object ID is ID |
| `archive/tags/ID/target` | Archived target revision |
| `archive/tags/ID/annotation` | Original annotation chunk when present |

Annotation and deletion records use existing chunk objects, compression and
encryption at rest. Their strict schemas bind names, target IDs and annotation
IDs. The archive also records a fresh nonce and deletion time. No object kind,
pointer, C ABI or physical storage format changes. Generic object/reference
transfers do not yet negotiate an atomic annotated-tag/archive group; dedicated
server tag policies and coordinated replication remain future work.

All commands support progress and cooperative cancellation. Mutations use the
existing complete-closure compare-and-swap reference publication. Cancellation
or an observer exception before publication keeps all old roots; newly written
unreferenced metadata chunks can remain for reuse or GC. After publication there
are no further cancellation points or callbacks. Process interruption leaves
either the old reference set or the fully published new set. Tag operations are
also allowed while a workspace checkout is pending; they do not alter its branch,
HEAD or internal recovery roots. Callbacks must not reenter the Store.

```cpp
auto selected = store.resolve_native_revision("HEAD", operation);
auto created = store.create_native_tag("release/v1", selected, "Release notes", operation);
auto page = store.native_tags({}, 100, operation);
auto shown = store.native_tag("release/v1", true, operation);
auto removed = store.delete_native_tag("release/v1", selected, operation);
auto archived_page = store.native_tag_archives({}, 100, true, operation);
auto saved = store.native_tag_archive(removed["archive"].get<std::string>(), true, operation);
auto restored = store.restore_native_tag(removed["archive"].get<std::string>(), operation);
```

The C ABI and Go binding do not expose native tag operations yet.
