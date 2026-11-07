---
id: native-watch
title: Resident native workspace observation
description: Resident native workspace observation guide.
section: Version control
order: 410
parent: native-history
revision: 1
---

# Resident native workspace observation {#resident-native-workspace-observation}

`native watch` keeps a stream of current workspace status. Windows uses
recursive directory notifications and Linux uses recursive inotify
registrations. Other platforms, explicit `--polling`, unavailable native
registration and exceeded registration limits use the polling backend.

```text
syrnck native watch
syrnck native watch --combined --branch main --progress
syrnck native watch --directory PROJECT --max-updates 10
syrnck native watch --polling --interval-ms 1000 --duration-ms 10000
syrnck native watch --audit-ms 60000 --debounce-ms 150 --max-paths 2048
syrnck native watch --journal --combined
syrnck native watch-journal
syrnck native watch-journal --clear
```

The command emits one compact JSON document per line and flushes each frame.
JSON keys, event types and reason codes are English in both interface
languages; error messages and stderr progress support English/French. It runs
until interrupted unless a duration or status-update limit is supplied.

This observes changes. It does not create commits, replace files, publish
references, transfer objects, acquire asset locks or resolve concurrent edits.
It is the watcher foundation for a future resident synchronization agent. The
[C ABI and Go workspace sessions](doc:workspace-abi) expose the same watcher and
inventory for embedding, with internally retained batches and caller-owned Store
handles. The CLI checkpoint journal is not exposed by those sessions.

## Status and event lifecycle {#status-and-event-lifecycle}

The watcher is armed before the initial query. Startup always clears the
disposable [working hash index](doc:working-index) and reads selected working
files completely. Consequently a restart observes edits made while the watcher
was offline, independently of an earlier cached hash.

Normal notifications are coalesced for a fixed bounded interval. Each batch
updates a complete in-memory inventory. Files and directory subtrees named by
native hints are reselected with the ordinary ignore rules. Regular files in
those scopes are read completely, even if their size and timestamps match;
unrelated subtrees are neither enumerated nor read. Targeted observations do
not rewrite the persisted hash cache. Creations, removals, renames and
file/directory transitions replace their complete inventory prefixes. A missing,
ignored or regular-file ancestor broadens the scope so stale sibling entries
cannot survive a deleted/replaced directory. Similar names such as `asset` and
`assets` remain distinct prefixes.

Overlapping hints coalesce to their enclosing scope. A nested `.syrnckignore`
change reselects its entire parent subtree, including nested rules and newly
included files. Root ignore changes still force a complete byte scan. Portable
case collisions, file/directory collisions and the v1 metadata size bound are
validated across retained and newly selected entries before hashing scoped
contents. Selection failures, case-spelling ambiguity, protected storage hints
and scope budget exhaustion fall back to a full scan; unresolved filesystem or
namespace errors remain errors. Symlink/reparse ignore files are refused by
both scan paths. Invalid ignore patterns produce a recoverable scan error.

Deletions and renamed paths are observed as
current filesystem state; event names are hints, not a transaction log or
guaranteed rename pairs. `--combined` preserves separate staging and working
changes, including stale staging. `--branch` or `--revision` follow the same
selection rules as native status; combined status cannot select a revision.

Native idle waits do not enumerate project directories or open the Store.
The Store exists only during a query and closes before output, waiting or
coalescing, so ordinary CLI operations can acquire its exclusive lock.
Activity in the local cache, object storage and `.git` does not feed back into
the observer. Changes to the actual Store's `refs.json` and `config.json` remain
visible. A differently spelled Store alias is checked for filesystem identity
on Windows; a separate `Vault` directory beside a custom `vault` Store on a
case-sensitive filesystem is not excluded.

Only changed semantic status, completed full scans and new errors produce
frames. Reused/hash-byte counters and cache generation changes alone do not
produce updates. The polling fallback enumerates the workspace at its query
interval, but unchanged semantic results are also quiet.

```json
{"version":1,"sequence":1,"type":"status","reason":"initial","backend":"windows","full_scan":true,"overflow":false,"paths":[],"status":{"version":1,"base":null,"clean":true,"tracked_files":0,"workspace_files":0,"hashed_bytes":0,"counts":{"untracked":0,"removed":0,"modified":0,"unchanged":0},"changes":[],"working_index":{"version":1,"mode":"stat-v1","previous":"missing","hashed_files":0,"reused_files":0,"supported_files":0,"cached_files":0,"truncated":false,"object":"INDEX_OBJECT_ID"}}}
```

The nested `status` has the ordinary or combined status fields. Working reports
also include `observation` with `mode: full|indexed|targeted`, `full_scan`,
`inspected_files` and `retained_files`. Targeted reports also include
`scanned_scopes`, the sorted replacement prefixes after overlap coalescing and
ancestor resolution. A removal can inspect zero files and hash zero bytes. Only enumerated queries include the
persisted `working_index` metrics; targeted queries retain a complete session
inventory, including files unsupported by the stat cache. A full fallback sets
the frame's `full_scan` flag as well. Report comparison still visits the
in-memory file maps and reads revision metadata; this is not constant-time
status for large histories.

`sequence` increases within this process, including error frames. Backends are
`windows`, `inotify` and `polling`. Status reasons are:

| Reason | Meaning |
| --- | --- |
| `initial` | First successful complete observation, including delayed startup |
| `filesystem` | A native notification changed semantic status |
| `polling` | A polling query changed semantic status |
| `audit` | Scheduled complete byte scan |
| `rescan` | Native registration/topology requires a complete byte scan |
| `overflow` | Notification/path information was lost or exceeded its budget |
| `recovered` | Complete scan after a previously available observation failed |

## Lost events, audits and unavailable observations {#lost-events-audits-and-unavailable-observations}

Notification overflow, malformed/incomplete native records, path-budget
overflow and native backend failure request a full scan and discard incomplete
path lists. The next query clears the hash cache and reads all selected bytes.
Non-UTF-8 native names also discard hints and request a full scan; diagnostic
display text replaces invalid encoding instead of breaking the JSON stream.
Linux directory creation and move-in events extend registrations on the same
inotify instance before returning their subtree hints. Each new directory is
armed before its children are enumerated; the subsequent complete observation
of that subtree covers earlier writes, and later events stay queued. Unrelated
registrations and descriptor/path mappings remain intact. Overlapping additions
coalesce, and the persistent directory count/name budget includes both existing
watches and queued additions.

Deletion, move-out, self-move, ignored-watch and unmount events still rebuild
the instance and request a complete byte scan. Existing descriptor paths are
never relabeled from rename cookies. Unknown descriptors, malformed records,
overflow, path/descriptor aliasing and failed/over-budget additive registration
also require recovery; unavailable/quota-limited registration can fall back to
polling. Partial native instances are closed before recovery, and cancellation
during extension invalidates continuity for the next wait. Native backend
failures can switch to polling.

Independent periodic audits also read all selected working bytes. Normal
notifications and polling queries do not postpone their schedule. These scans
cover notification limitations such as remote filesystem changes, missed
metadata notifications and memory-mapped writes. The default is five minutes;
large projects can choose another bounded interval.

An occupied Store, active recoverable checkout or failed scan emits an error
frame with `type: "error"`, `code: "scan_failed"`, a translated `message` and
`status: null`. The last successful observation is not represented as current.
Identical consecutive errors are suppressed. Retrying waits at least one
second, and successful recovery forces a full scan. The process holds no
Store lock while retrying. Permanent workspace-root identity changes require
restarting the watcher instead of silently following another directory.
The observer retains a directory handle for that identity for the session;
this does not prevent working writes or acquire a Store lock.

The command still observes a changing filesystem. It does not provide a locked
or atomic filesystem snapshot, does not preserve intermediate file versions,
and does not authorize other operations to skip their own validation.

## Optional persistent checkpoint journal {#optional-persistent-checkpoint-journal}

`--journal` records the current coalesced batch before processing native hints
or a full scan. It writes an `observed` checkpoint before emitting successful
status, or an `unavailable` checkpoint after a scan error when storage remains
accessible. A checkpoint is one bounded replacement record, not a replayable
history of individual filesystem writes. It contains the workspace and query
selection digests, random session identity, session generation, previous
session/state, backend, bounded path hints and the semantic status digest,
base revision, clean flag and file count. It does not store working contents
or the complete status report.

The stream additionally contains `journal: {session, generation, durable,
previous}`. `durable: true` means the corresponding checkpoint was saved before
output; it does not acknowledge delivery to the consumer. `sequence` remains
process-local. A process can terminate after checkpoint publication but before
stdout delivery, so consumers must reestablish current state after reconnecting.
A busy/unavailable Store can prevent error checkpointing; the error frame then
has `durable: false`. Corrupt journals are preserved and block journal mode
until explicitly cleared. Ordinary observation without `--journal` remains
available.

`native watch-journal` reads the last checkpoint, or reports `state: absent`.
An `observed` record means the last query finished, not that an observer is
currently alive or that its result is still current. A `pending` record can
remain after interruption. Startup always creates a new session and performs
a complete byte scan, whether the previous record was pending, observed or
unavailable; offline edits never resume from the old inventory. The first
checkpoint records the old session/state for diagnosis.

`native watch-journal --clear` explicitly removes the local record, including
malformed records within the file-size bound. It does not alter refs, staging,
objects or working files. Journal mode holds a separate `watch.lock` for the
session, preventing another journal owner or a clear while it runs. The Store
lock is still released between queries; checkpoint inspection and other CLI
operations remain available. Non-journal observers need no session lock.

The `watch-journal` file is self-contained: `SYW1`, a 64-byte chunk digest and
the existing compressed Store envelope. Payloads use the Store's existing
AEAD key when encrypted, including hint names. The digest and encryption mode
remain visible. The plaintext record is limited to 2 MiB, the stored file to
3 MiB, and paths to 65,536 entries with the same 1 MiB accounting budget as
coalesced hints. Writes use the normal temporary-file flush and atomic durable
replacement, with a publication fence after progress callbacks. There is no
object reference or temporary retained root; GC and pack compaction cannot
remove the checkpoint. Logical backups omit this machine-local record.

Native idle waits perform no checkpoint writes. Unchanged polling also keeps
the last observed checkpoint without pending writes or repeated flushes. Changed
polling results are checkpointed before output, and full audits write pending
then observed. The journal is recovery metadata for the observer; causal
versions, transfer outboxes and exactly-once network publication are separate
work.

## Limits and resource use {#limits-and-resource-use}

The workspace and Store directories must exist. An unreadable root or
unavailable directory identity is an error before observation starts.

| Option | Default | Accepted range |
| --- | --- | --- |
| `--interval-ms` | 500 | 20..60000; polling cadence and native loop wakeup cap |
| `--debounce-ms` | 75 | 0..5000; fixed coalescing window |
| `--audit-ms` | 300000 | 1000..3600000; complete working-byte audit interval |
| `--duration-ms` | 0 | 0 for unlimited, otherwise 100..86400000 |
| `--max-updates` | 0 | 0 for unlimited, otherwise 1..1000000 successful status frames |
| `--max-paths` | 4096 | 1..65536 unique hints per coalesced batch |

Duration limits are checked between observations; they do not turn an
individual filesystem call into a timed operation. Errors do not consume the
successful-update limit. Duration expiry returns 1 if the latest observation
remains unavailable, otherwise 0. Interruption uses the ordinary CLI
cancellation behavior.

Windows uses a DWORD-aligned 64 KiB notification buffer and a bounded completion
copy. Linux uses a 64 KiB read buffer, at most 65,536 directory registrations,
128 levels and 16 MiB of accounted relative names. Registration limits may
also be set lower by the OS. Both native hints and the coalesced CLI batch
have a 1 MiB path-accounting budget. Sets, path objects, maps, snapshots,
reports, kernel queues and allocation overhead are additional; these limits
are not a total RAM budget. Large output consumers must keep reading; queued
events can overflow while output is blocked, requiring a later full scan.

Scoped C++ observations accept at most 65,536 hints and 1 MiB of accounted hint
names. Each scope traversal accounts ancestor/scope directory names and ignore
bytes against 16 MiB; gathered selected names also have a conservative 16 MiB
budget. Crossing a scope budget requests a full scan. The ordinary full scan
and the retained+replacement union still enforce the v1 revision metadata
lower bound. These are accounting limits, not a total RAM or syscall budget.

## C++ API and validation {#c-api-and-validation}

```cpp
#include <syrnck/watcher.hpp>
syrnck::WorkspaceWatcher watcher(project, store_path, {}, stop);
// Perform the caller's initial complete observation after construction.
auto hints = watcher.wait(std::chrono::milliseconds(500), stop);
if (hints.full_scan) {
    // Reestablish a complete observation before trusting incremental hints.
}
watcher.invalidate(); // The caller lost its own observation state.
```

The API has one caller; cancellation may be requested from another thread.
`invalidate()` makes the next wait return a full-scan request immediately and
does not discard pending notifications. `wait` accepts 0..60000 milliseconds;
0 checks immediately. Native and polling waits check cancellation at most
50 ms apart, apart from filesystem/syscall latency. The [C ABI and Go sessions](doc:workspace-abi) expose workspace observation and watch batches; the CLI checkpoint journal remains CLI-only.

`WorkspaceObservation(project, store_path)` provides `status(store, revision,
hints, operation)` and `workspace_status(store, branch, hints, operation)`.
Arm a watcher first, feed every batch without gaps, and request full scans for
lost continuity. A new observation, `invalidate()`, any failed query and periodic
audits discard incremental assumptions. Polling enumerates with the persisted
stat cache between byte audits. Closing and reopening a Store does not lose the
session inventory, and collecting its unrooted cache objects does not truncate
that inventory. Mutation preflights continue to validate independently.


## Observation boundaries {#synthetic-performance-measurement}

File and subtree changes use targeted observation when continuity is preserved. Linux creates/imports extend registrations per subtree; removals and moves of existing watched directories still rebuild the instance and force a complete byte scan. Real Linux/macOS validation remains necessary. Replayable event history, causal versions, tombstones, transfer scheduling, OS service packaging and full Syncthing daemon/folder interoperability remain separate work. See [performance boundaries](doc:performance).

API references: [ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw), [inotify](https://www.man7.org/linux/man-pages/man7/inotify.7.html).
