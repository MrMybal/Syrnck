# Resident native workspace observation

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
```

The command emits one compact JSON document per line and flushes each frame.
JSON keys, event types and reason codes are English in both interface
languages; error messages and stderr progress support English/French. It runs
until interrupted unless a duration or status-update limit is supplied.

This observes changes. It does not create commits, replace files, publish
references, transfer objects, acquire asset locks or resolve concurrent edits.
It is the watcher foundation for a future resident synchronization agent.

## Status and event lifecycle

The watcher is armed before the initial query. Startup always clears the
disposable [working hash index](WORKING_INDEX.md) and reads selected working
files completely. Consequently a restart observes edits made while the watcher
was offline, independently of an earlier cached hash.

Normal notifications are coalesced for a fixed bounded interval. Each batch
updates a complete in-memory inventory. Edits to known regular files inspect
their ancestor directories and reread only those files, even if their size and
timestamps match. They do not enumerate unrelated subtrees or rewrite the
persisted hash cache. Creations, deletions, renames, directory/file transitions,
ignore-rule changes and uncertain targeted inspection use a complete byte scan
with the ordinary ignore rules, source selection and namespace checks.
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
`inspected_files` and `retained_files`. Only enumerated queries include the
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

## Lost events, audits and unavailable observations

Notification overflow, malformed/incomplete native records, path-budget
overflow and native backend failure request a full scan and discard incomplete
path lists. The next query clears the hash cache and reads all selected bytes.
Non-UTF-8 native names also discard hints and request a full scan; diagnostic
display text replaces invalid encoding instead of breaking the JSON stream.
Linux directory topology changes rebuild registrations and request a full scan
to cover the registration gap. Native backend failures can switch to polling.

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

## Limits and resource use

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

## C++ API and validation

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
50 ms apart, apart from filesystem/syscall latency. The C ABI and Go binding
do not yet expose this native workspace observer.

`WorkspaceObservation(project, store_path)` provides `status(store, revision,
hints, operation)` and `workspace_status(store, branch, hints, operation)`.
Arm a watcher first, feed every batch without gaps, and request full scans for
lost continuity. A new observation, `invalidate()`, any failed query and periodic
audits discard incremental assumptions. Polling enumerates with the persisted
stat cache between byte audits. Closing and reopening a Store does not lose the
session inventory, and collecting its unrooted cache objects does not truncate
that inventory. Mutation preflights continue to validate independently.

Local Windows validation covers native create/edit/delete/rename notifications,
Unicode paths, new and renamed directory trees, internal-noise filtering,
bounded overflow, cancellation and root replacement. CLI scenarios cover
plain/encrypted custom Stores, equal-size/restored-mtime edits, staging,
idle Store-lock availability, occupied Store recovery, periodic audits,
restart/offline edits, file/directory transitions, error suppression and
French diagnostics. Linux native execution and macOS fallback execution still
need confirmation by CI.

Existing-file edits use targeted observation; namespace changes still rebuild
directory inventories. A persistent change journal, incremental directory
topology updates, causal version
tracking, tombstones, transfer scheduling, OS service packaging and full
Syncthing daemon/folder interoperability remain separate work.

API references: [ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw),
[inotify](https://www.man7.org/linux/man-pages/man7/inotify.7.html).
