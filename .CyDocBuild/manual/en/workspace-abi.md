---
id: workspace-abi
title: Embedded workspace observation through C and Go
description: Embedded workspace observation through C and Go guide.
section: API
order: 640
parent: integrations
revision: 5
---

# Embedded workspace observation through C and Go {#embedded-workspace-observation-through-c-and-go}

The resident workspace watcher and its complete session inventory are available
through `SyrnckWorkspace` in the [C header](doc:api-c-abi) and `Workspace` in
the Go binding (`../bindings/go/workspace.go`). They use the same Windows/Linux
notifications, polling fallback and targeted/full observation as
[`native watch`](doc:native-watch). These are additive ABI 1 symbols; use the
header, binding and native library from the same release.

## Lifetime and locking {#lifetime-and-locking}

1. Create the Store and workspace directories first. For primary sessions, the Store must be strictly
   below the workspace root. Registered linked sessions use a disjoint Store
   and require their exact registered directory. Both paths are resolved at open.
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
Multiple successful waits and an ignored wait result do not lose hints. Native
cancellation invalidates continuity even if it interrupts batch processing; the
next successful status performs a complete byte scan instead of trusting a
partial inventory. Coalescing deduplicates paths, preserves
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

## C calls and output ownership {#c-calls-and-output-ownership}

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
| `0` | Branch name; NULL follows selected named or detached native HEAD |
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

## Cooperative cancellation and C progress {#cooperative-cancellation-and-c-progress}

The existing calls keep their signatures and delegate to `*_ex` with no
cancellation source/callback. Additional ABI 1 calls are:

- `syrnck_cancellation_create`, `syrnck_cancellation_request` and
  `syrnck_cancellation_close` manage one-shot sources.
- `syrnck_workspace_open_ex` and `syrnck_workspace_wait_json_ex` accept a
  nullable cancellation source, including recursive registration/recovery.
- `syrnck_workspace_status_json_ex` accepts a nullable `SyrnckOperation` with a
  source, synchronous progress callback and caller-owned user pointer.

Request is idempotent and may run concurrently with the using operation or
other requests. All using calls and requests must return before closing the
source. There is no reset: create a fresh source for another operation. Request
on NULL fails; close on NULL succeeds. No source is stored in the workspace.

Cancellation observed by the native operation returns `SYRNCK_CANCELLED` (3),
with NULL output and zero length. A cancelled open returns no handle. A cancelled
wait/status invalidates the session; retry without a requested source requires
a complete observation. A request after the final output check can follow a
successful result. Individual OS IO, filesystem metadata operations and JSON
encoding may finish before a cooperative checkpoint; there is no hard latency
bound. Cancellation can leave updated/cleared disposable hash caches or
collectible cache objects. References, staging, retained file data and working
contents are not changed by these advisory observations.

Progress callbacks execute synchronously on the caller's thread. `SyrnckProgress`
contains the stable `SyrnckProgressPhase` number, borrowed path bytes and
length, byte count, optional total, completion flag and buffered-block count.
Paths are not necessarily NUL terminated. Copy anything needed after the callback
returns; neither the event nor the callback/user pointer is retained. Never
reenter/close the Store or workspace, throw, or unwind through the ABI from a
callback. It may call `syrnck_cancellation_request` safely. These callbacks are
available in C; the Go context API does not yet expose progress callbacks.

```c
static void stop_after_first_chunk(const SyrnckProgress *event, void *user) {
    if (event->phase == SYRNCK_PHASE_VERIFYING && event->bytes >= 1024 * 1024)
        syrnck_cancellation_request((SyrnckCancellation *)user);
}
/* Keep cancellation and operation alive for the complete status call. */
SyrnckOperation operation = {cancellation, stop_after_first_chunk, cancellation};
int result = syrnck_workspace_status_json_ex(workspace, store, NULL, 0,
                                            &operation, &json, &length);
/* Inspect/copy diagnostics first. Close cancellation only after all request
   threads have also returned. Free json only when the result owns it. */
```

## Go usage {#go-usage}

`OpenWorkspace`, `OpenWorkspaceContext`, `Wait`, `Status`, `StatusContext`,
`Invalidate` and `Close` wrap those operations.
`Wait` returns typed `WorkspaceChanges`; `Status` returns `json.RawMessage` so
all current status fields remain available. Its zero `WorkspaceSelection`
follows HEAD, `Branch` overrides it, `Revision` selects an exact/resolved
snapshot and `Combined` separates staging from work. Revision excludes the
other two options. `Close` is idempotent; calls on a closed workspace return
`ErrWorkspaceClosed`.

```go
w, err := syrnck.OpenWorkspaceContext(ctx, project, storePath, syrnck.WorkspaceOptions{})
if err != nil { return err }
defer w.Close()

s, err := syrnck.Open(storePath, syrnck.Options{Key: key})
if err != nil { return err }
report, err := w.StatusContext(ctx, s, syrnck.WorkspaceSelection{Combined: true})
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
0..one minute and passes cancellation into a single native wait. Native idle
waits check stop requests about every 50 ms; polling becomes actionable at the
requested deadline. Registration and status hashing also use cooperative stop
checks. `OpenWorkspace` and `Status` use `context.Background()` for compatibility.

Context cancellation maps native status 3 to `context.Canceled` or
`context.DeadlineExceeded`. The callback that requests the native source is
stopped/joined before the source is released. Each call owns a fresh source;
a late request cannot cancel a later query. A completed native success/error
wins over cancellation arriving after its final check. A context that expires
before native execution returns its error without starting the operation.

Wait/StatusContext can also abandon the workspace/Store serialization wait when
the context expires. Lock order remains workspace-then-Store, with the existing
stable Store order retained by differential copying. Close still waits for any
active call, so cancel a long call before closing. Ordinary Store methods keep
their existing cancellation behavior; this context bridge currently applies to
workspace opening, waiting, status and the guarded native actions below.

## Scope and validation {#scope-and-validation}

This is advisory status, not a locked filesystem snapshot or a transfer agent.
It does not preserve intermediate file versions, maintain a replayable event
history or expose the CLI's persistent checkpoint journal. A restart always
creates a new inventory and performs a complete scan. Service lifecycle,
causal versions and transfers are managed above this observer; see the separate
[continuous BEP controller](doc:syncthing#folder-controller) and its remaining
folder/conflict/service limits.

## Registered contexts {#registered-contexts}

Use `syrnck_workspace_open_registered_ex(directory, store_path, name, options,
cancellation, out)` for a registered linked workspace. Before each status,
select the same context on the caller-opened Store with
`syrnck_store_use_workspace(store, name)`. NULL or empty selects primary.
An unknown name fails without changing selection; mismatched sessions/Stores
fail without returning stale output. Never change selection from a progress
callback. These additive symbols require matching rebuilt ABI 1 libraries.

Go uses `WorkspaceOptions{Name: "edit"}` at open and `store.UseWorkspace("edit")`
before status. The session retains no Store/key/lock. Linked native watchers
observe external refs/config metadata conservatively and require periodic full
audits as primary sessions do. Registration and portable restore are exposed by
the [CLI and C++ API](doc:native-workspaces), not by C/Go management calls.

## Guarded native workspace actions {#native-workspace-actions}

The additive C ABI also exposes `workspace_native_state_json_ex`,
`workspace_capture_json_ex`, `workspace_checkout_plan_json_ex`,
`workspace_checkout_owned_json_ex` and `workspace_recover_owned_json_ex`.
Use the distributed declarations for the complete signatures. Each call binds
its supplied Store to the session's exact canonical storage path, registered
context and directory; even another valid Store is refused.

Read state first. The report includes selected `target`, its actual reference,
typed `revision` or null, HEAD, materialization policy, staging flag and the
selected context's checkout status. Reading state during an interruption does
not recover it. `SyrnckNativeExpected` contains target, nullable revision and
nullable materialization object ID. NULL means absence, not a wildcard. Capture,
plan and checkout refuse a changed target, revision or policy before mutations.
Never update an old expectation merely to force the same operation through.

Capture retains selected working files through the ordinary native commit
transaction. It preserves ignored files, unmaterialized paths, detached HEAD
and authored-commit behavior, and refuses existing staging/checkout work. The
message is at most 64 KiB. Plan verifies the target closure and reports readiness
without moving refs or changing working bytes. A blocked plan is a successful
report with `ready:false`. Checkout rechecks those conditions and applies only
to a clean selected workspace through the recoverable native journal. It uses
an explicit 64-hex owner identity and the existing reader feature gate.

Before starting a service-owned checkout, durably retain its exact owner,
candidate and causal input. Recovery accepts only a currently pending journal
with that owner, candidate, selected target, context and physical workspace.
A foreign/manual journal or no pending journal is refused. Recovery does not
require the private key that signed an already prepared target. Signed target
objects and their signatures stay unchanged.

All methods share allocated output ownership and workspace-then-Store locking.
Mutation attempts invalidate observation continuity; the next Status reads all
selected bytes. Native cancellation remains cooperative. A successful native
publication wins late cancellation. A nonzero result can follow publication or
leave a resumable journal: read native state and establish the outcome before
retrying or recovering. Do not infer absence of changes from empty error output.
Close Store before waiting for notifications or network responses.

Go provides `NativeState`/`NativeStateContext`, `Capture`/`CaptureContext`,
`PlanCheckout`/`PlanCheckoutContext`, `CheckoutOwned`/`CheckoutOwnedContext` and
`RecoverOwned`/`RecoverOwnedContext` on Workspace. `state.Expected()` carries
its exact target/revision/materialization snapshot. Capture returns a revision
ID; plan/checkout/recovery return canonical `json.RawMessage`. Context methods
use the same safe per-call cancellation bridge. They do not implement a resident
BEP controller or automatically announce that a working folder has caught up.

`syrnck_workspace_checkout_status_json_ex(workspace, store, transaction,
operation, out, length)` reads one exact 64-hex transaction through the core
journal/receipt decoder, without recovery or publication. A completed receipt
returns pending=false and state=committed, plus its original owner/source/target/
branch/context/directory. It verifies the matching retained target and bound
workspace; completed lookup does not diff historical file trees. These facts
prove past application, not current working cleanliness. Go exposes this as
`CheckoutStatusContext`. Use a unique durable owner per pending application,
retain its causal inputs before checkout and capture later local edits after
acknowledging that completed base.
