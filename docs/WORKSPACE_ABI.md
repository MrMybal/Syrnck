# Embedded workspace observation through C and Go

The resident workspace watcher and its complete session inventory are available
through `SyrnckWorkspace` in the [C header](../include/syrnck.h) and `Workspace` in
the [Go binding](../bindings/go/workspace.go). They use the same Windows/Linux
notifications, polling fallback and targeted/full observation as
[`native watch`](NATIVE_WATCH.md). These are additive ABI 1 symbols; use the
header, binding and native library from the same release.

## Lifetime and locking

1. Create the Store and workspace directories first. The Store must be strictly
   below the workspace root; both paths are resolved when opening the session.
2. Open the workspace session before its initial status. Native notifications
   are already armed when open returns. The session retains its root directory
   identity, watcher and in-memory inventory, with no Store handle or Store lock.
3. Open the Store only for a status query, supplying its encryption key normally.
   Query then close the Store before consuming output or waiting. The session
   does not retain the Store handle or its key.
4. Wait for changes, then reopen the Store for the next status. The first query,
   explicit invalidation and recovery read all selected working bytes. Ordinary
   native hints select complete file/subtree prefixes; polling enumerates with
   the disposable working hash cache.
5. Schedule independent complete audits, for example every five minutes, by
   invalidating before the next query. Idle notifications do not prove that all
   filesystem changes were reported. Close the workspace explicitly at shutdown.

Calls on each C handle must be serialized, including close. The Go wrapper
serializes workspace calls with its own mutex and locks the Store during status,
using the workspace-then-Store order. A caller can still keep a Store open while
waiting; that caller would keep its exclusive Store lock unnecessarily.

The session retains every batch consumed by a wait until a successful status.
Multiple waits, an ignored wait result and a Go wait cancelled after consuming a
native batch do not lose its hints. Coalescing deduplicates paths, preserves
full-scan/polling flags and requests a full scan after exceeding the configured
path count or 1 MiB accounting budget. Failed native waits, status queries and
JSON allocation/serialization invalidate inventory continuity. Pending hints are
cleared only after successful status serialization. A changed root identity
requires closing/reopening the session; it never silently adopts another root.

Notification delivery is asynchronous. Status collects one currently ready batch;
an edit whose notification has not arrived can still be absent from that report.
Wait for the relevant hints before requesting event-driven status. Call
`Invalidate` before a synchronous complete byte comparison instead. Observations
remain advisory while the filesystem is changing; they authorize no destructive
workspace operation to skip its own preflight.

## C calls and output ownership

| Call | Effect |
| --- | --- |
| `syrnck_workspace_open` | Open/arm; NULL options means native notifications with polling fallback, 4096 hints and 65536 directory registrations |
| `syrnck_workspace_wait_json` | Wait once for 0..60000 ms, retain the batch and return its hints |
| `syrnck_workspace_status_json` | Collect one immediate native batch and observe all retained hints against a caller-opened Store |
| `syrnck_workspace_invalidate` | Require a complete byte scan on the next status |
| `syrnck_workspace_close` | Release native resources and inventory; NULL is accepted |

Supplied `SyrnckWorkspaceOptions` has three `uint32_t` fields: `polling` (0/1),
`max_paths` and `max_directories` (each 1..65536). C zero limits are invalid;
NULL options selects all defaults. Go zero limits individually select defaults.

Both JSON calls return an allocated `char *` plus a length including its trailing
NUL. Release it exactly once with `syrnck_string_free`, including results that
are no longer needed. Output is capped at 128 MiB plus NUL. There is no
size-query/re-execution pair for these stateful calls. Output pointers and
lengths reset to NULL/zero on failure when supplied; copy thread-local diagnostic
text before another ABI call. A failed query never returns an older status as
current.

Wait JSON has `version: 1`, `backend: windows|inotify|polling`, `paths`,
`full_scan`, `overflow` and `polling`. It describes that native wait, while the
session may retain earlier batches too. Status JSON has the same schema as the
ordinary or combined native status report, including working-index/observation
metrics. Full fallback is visible in the working report's
`observation.full_scan`, including when a coalesced budget triggered recovery.

Status selection modes are:

| Mode | Selector |
| --- | --- |
| `0` | Branch name; NULL follows the selected native HEAD branch |
| `1` | Required revision selector, resolved by the native revision resolver |
| `2` | Combined staging/working status; branch name or NULL for selected HEAD |

An explicit empty selector is invalid. Unknown working branches have an empty
baseline, matching ordinary status. The Store must be the one used at session
open. Active recoverable checkout and HEAD bound to another workspace are
refused. Status can update the disposable hash cache; it does not ingest working
contents, publish a revision or change staging/references.

```c
/* After opening a session and caller-owned Store: */
char *json = NULL;
size_t length = 0;
int result = syrnck_workspace_status_json(workspace, store, NULL, 0, &json, &length);
/* On failure, copy syrnck_last_error() before close/free/another ABI call. */
syrnck_store_close(store);
if (result == 0) {
    consume_status(json, length - 1); /* consume while allocation is alive */
    syrnck_string_free(json);
}
/* The next wait holds no Store lock. Free its JSON result in the same way. */
```

## Go usage

`OpenWorkspace`, `Wait`, `Status`, `Invalidate` and `Close` wrap those operations.
`Wait` returns typed `WorkspaceChanges`; `Status` returns `json.RawMessage` so
all current status fields remain available. Its zero `WorkspaceSelection`
follows HEAD, `Branch` overrides it, `Revision` selects an exact/resolved
snapshot and `Combined` separates staging from work. Revision excludes the
other two options. `Close` is idempotent; calls on a closed workspace return
`ErrWorkspaceClosed`.

```go
w, err := syrnck.OpenWorkspace(project, storePath, syrnck.WorkspaceOptions{})
if err != nil { return err }
defer w.Close()

s, err := syrnck.Open(storePath, syrnck.Options{Key: key})
if err != nil { return err }
report, err := w.Status(s, syrnck.WorkspaceSelection{Combined: true})
closeErr := s.Close()
if err != nil { return err }
if closeErr != nil { return closeErr }
if err := consumeStatus(report); err != nil { return err }

changes, err := w.Wait(ctx, time.Second)
if err != nil { return err }
// Reopen s and query again when changes.Changed(), or when an audit is due.
_ = changes
```

Go validates UTF-8/NUL inputs, normalizes zero option limits, serializes calls,
copies native output into Go memory, frees native allocations and reads
thread-local errors on the same OS thread as the failed call. Wait accepts
0..one minute and checks its context between native waits of at most 50 ms.
Polling becomes actionable at the requested deadline, rather than every internal
slice. Close waits for any active call; cancel a long idle Wait first.

The context does not interrupt native directory registration/recovery already
underway. Status hashing and registration are not yet cancellable/progress-aware
through this ABI. The C++ APIs retain their existing stop-token support.

## Scope and validation

This is advisory status, not a locked filesystem snapshot or a transfer agent.
It does not preserve intermediate file versions, maintain a replayable event
history or expose the CLI's persistent checkpoint journal. A restart always
creates a new inventory and performs a complete scan. Service lifecycle,
causal versions, tombstones, conflict resolution and full Syncthing daemon
interoperability remain separate work.

Local Windows tests exercise static/shared C linking, output ownership/null
failures, native and polling observation with plain/encrypted Stores, retained
batches, targeted reads beside a 4 MiB file, coalesced overflow, revision/combined
selection, query failure recovery, explicit audits, Store reopening, context
cancellation, concurrent Go calls and closed handles. The Go tests also run with
`-d=checkptr=2`. Linux/macOS cgo and native execution require the existing CI
matrix; Windows DLL tests do not validate those backends.
