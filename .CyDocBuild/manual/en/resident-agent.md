---
id: resident-agent
title: Resident native publication agent
description: Watch a native workspace, capture local changes and resume guarded publications.
section: Transfers
order: 405
parent: transfers
revision: 1
---

## Run continuously {#run}

```text
syrnck agent run https://blocks.example.invalid --directory PROJECT --remote-branch main --require-lease
syrnck --workspace edit agent run https://blocks.example.invalid --remote-branch feature --require-lease
syrnck agent run https://blocks.example.invalid --commits-only --polling
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
`--commits-only` publishes existing local commits without creating revisions or
altering dirty working files. The agent never replaces working files with remote
content.

This is the resident **publication direction**, not a complete bidirectional
folder synchronization service. Remote descendants or concurrent histories
require intervention; no conflict copy or merge is silently selected. Full
causal reconciliation, OS service installation and complete Syncthing daemon
interoperability remain unfinished. See [current limits](doc:limits).

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
of the local HEAD may be advanced; remote descendants and divergent histories
are blocked.

If an earlier publication has an unknown outcome, its original durable session
must reconcile first, even when local HEAD has since advanced. A receipt proving
past success does not authorize overwriting a later remote change. A recorded
transfer conflict requires explicit resolution; the agent never discards that
journal automatically. After resolving the underlying history or policy issue,
the process retries. An incomplete checkout or occupied staging index blocks
capture and publication until it is completed or cleared.

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
`state` is `empty`, `pushed`, `synced`, `uncommitted`, `blocked` or `unavailable`.
Canonical reason codes distinguish staging, checkout, remote/local rewrites,
deleted references, concurrent histories and uncertain publications. Frames
include local/remote revision IDs and the acknowledged baseline when known.
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

syrnck::net::NativePushSync sync(store, project, {"main", true});
auto status = sync.tick(store, remote, watcher_changes, operation, transfer);
```

Keep this move-only session and its observation state between ticks. Every tick
uses a currently locked Store handle with the same physical Store, logical
workspace, HEAD target and endpoint. Call `invalidate()` when watcher continuity
is lost; a new session starts with a full audit. The caller owns scheduling and
cancellation. There is no background thread or implicit CLI profile loading in
this C++ API. Link `syrnck_net` and provide any signing identity explicitly.
The CLI uses `Store::take_native_identity()` to move exclusive key ownership
between short-lived handles without exporting seed bytes.
