---
id: resident-agent
title: Resident native synchronization agent
description: Watch native workspaces, exchange guarded revisions and merge compatible histories.
section: Transfers
order: 405
parent: transfers
revision: 3
---

## Run continuously {#run}

```text
syrnck agent run https://blocks.example.invalid --directory PROJECT --remote-branch main --require-lease
syrnck --workspace edit agent run https://blocks.example.invalid --remote-branch feature --require-lease
syrnck agent run https://blocks.example.invalid --commits-only --polling
syrnck agent run https://blocks.example.invalid --bidirectional --require-lease --require-read-lease
```

These examples require an initialized Store, a real server and `SYRNCK_TOKEN`.
`--store PATH` selects the Store. Without `--directory`, the primary workspace
uses the current directory; a selected linked workspace uses its registered
directory. The current named or detached HEAD is frozen for the session.
`--branch NAME`, when supplied, must match it. The remote branch defaults to
`main`; remote names are single components of at most 64 bytes.

By default, the agent observes selected working files and creates a native
snapshot when their content changes. This includes deletions and exact renames.
Partial workspaces preserve the unmaterialized files from their complete parent
revision. A change to multiple files is not an atomic filesystem snapshot:
keep external writers stable when a consistent application snapshot is needed.
`--commits-only` disables automatic working-file snapshots. In the default
publication direction it only publishes existing commits and never changes
working files. With bidirectional mode, existing histories may still produce
a merge revision as described below.

Complete unattended folder synchronization, automatic conflict copies,
OS service installation and complete
Syncthing daemon interoperability remain unfinished. See [current limits](doc:limits).

## Exchange revisions in both directions {#bidirectional}

`--bidirectional` explicitly permits incoming working-file changes. A clean empty
workspace can receive its first remote revision. Remote descendants are applied
through the existing recoverable checkout transaction, preserving the previous
head and honoring partial materialization and detached HEAD.

Concurrent descendants of a shared history use a three-way merge. Compatible
file changes and non-overlapping text edits can produce a signed two-parent
revision, which is published against the exact observed remote root. Text
composition uses the existing bounded merge engine, including the batch limit
of 64 attempts and 16 MiB of inputs/results. No unrelated histories or ambiguous
merge bases are selected automatically. A remote rewrite outside the acknowledged
history is blocked even if a merge could otherwise combine it.

Unresolved binary, overlapping-text and topology conflicts produce a blocked
status with the merge plan. Both local and fetched histories remain retained;
working bytes are preserved. Resolve the conflict with the normal
[native merge workflow](doc:native-resolutions); the agent retries the resulting
local history. Automatic conflict-copy naming is not implemented.

With `--commits-only --bidirectional`, reception and merges still require the
normal clean-workspace policy: dirty files are left intact and block application.
An edit arriving during a preflight can also block the checkout. Watcher hints
never authorize replacement without that fresh verification.

## Publication and recovery {#recovery}

Each publication uses the exact observed remote revision as an expected-value
guard. Complete verified closures, resumable [transfer sessions](doc:transfer-sessions),
server permissions, approved signing keys and optional asset-lock claims apply
as for ordinary transfers. Use `--require-lease` to require negotiated server GC
protection; the agent does not acquire or renew asset-lock claims automatically.
Supply current claims with `--lock-claims PATH` where branch policy requires them.

A successful publication records a typed baseline root and a descriptor under
the selected workspace's `sync/push/SERVICE/` references. The descriptor binds
the logical workspace, HEAD target, remote branch and normalized endpoint
fingerprint. It contains no token or historical filesystem destination. Replaced
baselines remain protected by `archive/agent-push/SERVICE/REVISION` roots. GC and
logical backups therefore preserve their complete reconstruction dependencies.
Quiet polls and restarts do not rewrite an unchanged checkpoint.

Remote deletion or rewind after an acknowledged publication is blocked instead
of being undone. A rewritten local history is also blocked. An unseen remote
revision is fetched into a separate `remotes/agent-…/BRANCH` tracking reference,
without switching HEAD or changing working files. A remote that is an ancestor
of the local HEAD may be advanced. The default publication direction blocks
remote descendants and divergent histories; `--bidirectional` enables the
reception and compatible merges described above.

If an earlier publication has an unknown outcome, its original durable session
must reconcile first, even when local HEAD has since advanced. A receipt proving
past success does not authorize overwriting a later remote change. A recorded
transfer conflict requires explicit resolution; the agent never discards that
journal automatically. After resolving the underlying history or policy issue,
the process retries. Bidirectional mode can [resume its own interrupted checkout](doc:native-checkout#owned-recovery)
offline, without re-signing its frozen target. Manual, other-session and
publication-only checkouts stay blocked. An occupied staging index blocks capture
and publication until it is cleared.

The typed `sync/push/SERVICE/observed` reference records the latest confirmed
remote history before incoming file application and before completing a push
session. Earlier observations remain under `archive/agent-observation/` roots.
Deletion or rewind of that observed history is blocked even when a crash happened
before the acknowledged checkpoint. An initial service can import surviving
completed native transfer receipts as historical facts. These facts never
authorize overwriting a later remote change. `resident-sync-v1` gates this state
before any publication; it remains enabled after cleanup or mode changes.

Offline local edits can still become signed local snapshots. Reconnecting sends
their missing blocks. Restarting rescans the selected workspace and reuses
durable revisions, checkpoints and transfer sessions. The signing identity is
loaded once at startup and retained in process memory; changing a profile or
rotating its seed requires restarting the agent. See [identity setup](doc:native-authorship).

## Scheduling and output {#controls}

| Option | Default | Accepted values |
| --- | --- | --- |
| `--interval-ms` | 500 | 20..60000 |
| `--debounce-ms` | 75 | 0..5000 |
| `--audit-ms` | 300000 | 1000..3600000 |
| `--remote-poll-ms` | 5000 | 100..3600000 |
| `--duration-ms` | 0 | 0 for continuous, or 100..86400000 |
| `--max-updates` | 0 | 0 for unlimited, or 1..1000000 |
| `--max-paths` | 4096 | 1..65536 |

Native notifications are hints; periodic audits and restart scans recover lost
continuity. `--polling` uses conservative rescans. Errors retry after one second.
The Store is released before waiting, debouncing or writing a status frame, so
other CLI commands can run between cycles. Scans and network transfers still
hold its exclusive lock; large transfers can delay other commands.

Standard output contains NDJSON frames only when semantic status changes.
`state` is `empty`, `pushed`, `pulled`, `recovered`, `synced`, `uncommitted`, `blocked` or `unavailable`.
Canonical reason codes distinguish staging, checkout, remote/local rewrites,
deleted references, concurrent histories and uncertain publications. Frames
include local/remote revision IDs and the acknowledged baseline when known.
`last_observed_remote` reports retained knowledge even before that checkpoint.
`merged: true` identifies an automatically composed local merge; checkout/merge
reports describe incoming application or blockers when relevant.
`working_clean: null` means a new audit is required, including immediately after
an automatic snapshot. Human messages follow `--lang en|fr`; IDs and JSON keys
remain unchanged. `--progress` or `--progress-json` writes progress to stderr.

At a configured duration/update limit, exit status reflects the last cycle:
0 for an ordinary state, 1 for an unavailable operation, 2 for a blocked state,
3 for an uncertain publication. Cooperative cancellation returns 130. A duration
is a scheduling limit, not an immediate deadline for in-flight IO.

## Embed the publication direction {#cpp}

```cpp
#include <syrnck/native_sync.hpp>

syrnck::net::NativeSync sync(store, project, {"main", true, true, true});
auto status = sync.tick(store, remote, watcher_changes, operation, transfer);
```

Keep this move-only session and its observation state between ticks. Every tick
uses a currently locked Store handle with the same physical Store, logical
workspace, HEAD target and endpoint. Call `invalidate()` when watcher continuity
is lost; a new session starts with a full audit. The caller owns scheduling and
cancellation. There is no background thread or implicit CLI profile loading in
this C++ API. The options are remote branch, local capture, reception and
compatible merges; reception and merges default to false. Merge mode requires
reception. The earlier `NativePushSync` / `NativePushSyncOptions` names remain
valid. Link `syrnck_net` and provide any signing identity explicitly.
The CLI uses `Store::take_native_identity()` to move exclusive key ownership
between short-lived handles without exporting seed bytes.
