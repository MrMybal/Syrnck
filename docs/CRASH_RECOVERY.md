# Process interruption and recovery

Syrnck publishes complete objects before references, and complete verified packs
before replacing the active catalog. Maintenance holds its exclusive store lock
until it returns. The recovery tests kill real native writers between these steps
and check the resulting store with the production core in fresh processes.

## Tested interruption boundaries

The writer pauses at a selected checkpoint and acknowledges it over a pipe.
Python then uses `Popen.kill()`: `TerminateProcess` on Windows and `SIGKILL` on
Unix. A destructor sentinel must remain absent. These tests do not simulate a
crash by throwing an exception or unwinding the stack.

| Operation | Boundaries and checks |
| --- | --- |
| Revision publication | Parallel ingestion, chunk/manifest/revision writes and reference replacement; previous revision and archive remain reconstructible |
| Native staging | Selected chunk/manifest/snapshot writes, atomic branch/index commit and index reset; prepared bytes survive, with either the old head plus index or the new head without index |
| Native staging rebase | Candidate/reference writes for independent replay, explicit conflict resolution, clean results and initial empty baselines; text composition also covers generated chunk/manifest writes; the former index archive and replacement publish together, without changing the branch or dirty working files |
| Native branch lifecycle | Branch creation, deletion with/without a frozen index and restoration under original/new names; archive-record and renamed-index writes, atomic head/index/archive publication, retained historical data and unchanged symbolic HEAD/dirty working files |
| Native amendment | Directory chunk/manifest writes and all three modes' revision/reference publication; branch, previous-tip archive and staged-index consumption publish together, preserving parents and retained bytes |
| Native branch reset and squash | Soft/mixed reset with/without prepared bytes, partial/root squash, candidate staging/revision writes and atomic branch/index/history/index-archive publication; dirty working files remain untouched and retained data survives retry and GC |
| Native cherry-pick, revert and abort | Candidate revision writes and the checkout journal/prepared-file/move/publication/rollback boundaries; replay creates a child of the old tip and retains old file bytes through resume, abort and GC |
| Native merge and abort | Ordinary and explicitly resolved two-parent candidates, exact fast-forward, the checkout publication/move/rollback boundaries and both-parent retention through recovery and GC |
| Native rebase and abort | Two-candidate linear chains, resolved conflicts, successive virtual text compositions and empty-range moves; first/second generated chunk/manifest/candidate writes, both prepared files and moves, checkout journal/publication/rollback boundaries, stable planned IDs and unchanged onto roots |
| Native checkout and abort | Journal/pending roots, prepared files, exclusive backup/install/rollback moves, branch/archive/receipt publication and cleanup; resume or rollback preserves the selected version and leaves no active journal |
| Native namespace checkout and abort | File/directory transitions and parent case changes in one operation; all-source evacuation, first/second directory removal/creation, all prepared files and backup/install/rollback moves, durable uninstall/restore phases and exact-name retention |
| Local asset leases | First/second lease records and persistent fencing counter writes; atomic grouped acquisition, renewal, release and expiry-pruning publication |
| Native partial text staging | Selected chunk/manifest/index writes and atomic publication, with or without a previous index archive; unselected text, other frozen files and working bytes stay intact |
| Native partial text reset | Chunk/manifest/replacement-index writes and atomic old-index archival; removed text stays reconstructible in the archive, remaining prepared text and working files stay intact |
| Native individual line selection | Partial add with/without an existing index and partial reset inside a hunk; selected-row chunk/manifest/index writes, atomic retention, untouched working files and exact independently computed result identities |
| External native text patches | Strict single-file patch against frozen head/index, with/without an old index; chunk/manifest/index publication, atomic archive retention, exact independent result identity and unchanged working bytes |
| Native branch switch and abort | HEAD descriptor/journal writes, the same file-move boundaries, HEAD/receipt publication and rollback; both branch heads remain unchanged and HEAD publishes with the committed journal |
| Native tags | Annotation/deletion-record writes and atomic tag/annotation/archive reference creation, deletion and restoration; the old or complete new root set retains target bytes |
| History detachment | Both reference writes and new revision publication; archive precedes branch replacement |
| Incremental packing | Pack/index staging, second-pack construction, catalog replacement and loose cleanup; existing packs remain byte-identical |
| Compaction | Staging, catalog replacement, loose and obsolete-file cleanup; every logical object survives |
| Garbage collection | Partial pack rewrite, wholly dead pack removal, retained pack preservation, loose cleanup and restart |
| File reconstruction | Staging and destination publication; destination is absent or byte-complete |
| Native directory export | File staging, directory creation, completed tree, directory synchronization and exclusive publication; destination is absent or complete |

Atomic writes are interrupted after temporary-file opening, writer completion,
file synchronization, publication and completion of existing durability calls.
Pack construction also stops between envelopes. Unix adds the interval between
creating a destination hard link and unlinking its temporary name. Later
occurrences cover partial multi-pack and multi-object progress.

Fixtures contain shared/repeated blocks, retained archives, unreachable metadata,
mixed loose/packed objects, physical duplicates and wholly/partially live packs.
Every case runs with plaintext and encrypted objects, with decoded and index-page
caches disabled during recovery. Unicode paths exercise Windows argument handling.

After every kill, an executable linked only to the ordinary production core must
acquire the released lock, validate all retained closures and reconstruct current
and archived bytes. Reading must not change stored files. The interrupted
operation is retried in another production-core process, followed by a complete
audit. Packing and GC must clean inactive generated pack/index files. A positive
control releases the checkpoint normally and confirms that another process
cannot open the locked store while the writer is paused.

The local Windows x64 Release run on 5 November 2026 passed **1,932 process-kill
combinations**, plus checkpoint/lock and production-isolation checks. Unix adds
hard-link-boundary cases. Partial text staging adds 80 Windows combinations
(2,012 configured combinations total). A focused local run passed all 80 new
cases plus 2 commit checkpoints, production-isolation and release/lock controls.
Partial text reset adds 40 more Windows combinations (2,052 configured total).
A separate focused local run passed all 40 new reset cases plus 2 commit
checkpoints, production-isolation and release/lock controls. Individual line
selection adds 120 Windows combinations (2,172 configured total). Its focused
local run passed all 120 new cases, all 120 existing partial-hunk cases and
2 commit checkpoints, plus production-isolation and release/lock controls.
External patch staging adds 80 Windows combinations (2,252 configured total).
Its focused local run passed all 80 new cases, all 240 existing partial-hunk/
line cases and 2 commit checkpoints, plus production-isolation and release/lock
controls. The complete 2,252-case campaign has not been rerun for these additions.
This local result does not claim the new suite has
already run on Linux, macOS, ARM64 or sanitizer builds. CTest includes it
automatically in those configured builds.

A focused Windows x64 Release run on 6 November 2026 passed all **236 new text
rebase/index process-kill combinations**, plus 6 existing rebase/index/commit
checkpoints (**242 total**), production-isolation and release/lock controls.
The linear fixture composes the same text file in two successive virtual steps;
fresh production readers verify both intermediate and final generated manifests,
the source history, the two-file checkout and resume/abort bytes. The index
fixture verifies generated text, atomic old-index archival and unchanged dirty
working files. Cases run with plaintext and encrypted objects. This focused run
does not claim a new complete-campaign, other-platform or power-loss result.

A second focused Windows x64 Release run on 6 November 2026 passed **262
process-kill combinations**, including 256 namespace checkout/abort combinations
and 6 existing checkout/rebase/commit checkpoints. The fixture combines a file
becoming a directory, a directory becoming a file, a parent case change and an
ordinary binary replacement. Fresh production readers verify exact path spellings,
bytes, archives and complete resume/rollback in plaintext/encrypted storage.
Isolation and exclusive-lock controls also passed. This is a focused process
termination result, without a claim of power-loss or other-platform validation.

A focused Windows x64 Release asset-lease run on 6 November 2026 passed **94
process-kill combinations**, including 90 new grouped acquisition, renewal,
release and pruning combinations, plus 4 existing checkout/commit checkpoints.
Fresh production readers verify complete owner sets, matching generations and
deadlines, retained asset history and retry. Plaintext/encrypted cases and
production-isolation/exclusive-lock controls passed. This is separate from the
larger full campaign and other-platform or power-loss validation.

Exports may leave `.syrnck-export-<32 hex>` sibling directories after a hard
kill. Recovery verifies the published tree if present; a retry prepares a fresh
tree if the destination is absent. It does not remove old staging directories
automatically. See [export recovery and publication guarantees](EXPORT.md).

Native checkout persists an active journal and source/target pins before working
file moves. Opening the Store leaves working files untouched; use
`native checkout-status` and `native checkout-recover` (or `--abort` before
publication). Completed branch publication can only finish cleanup. See
[checkout recovery, retention and private-data limits](NATIVE_CHECKOUT.md).

## Interrupted garbage collection

GC protects the complete closure of the references and pins present when it
runs. Unreferenced metadata and its unreachable dependencies are eligible for
deletion.

If the process stops after publishing a GC catalog but before loose cleanup
finishes, an unreachable loose manifest may remain after its dead packed chunks
have left the active catalog. Interruption during loose deletion can also leave
dead metadata whose dead chunks have already been removed. `fsck` audits
unreachable metadata too, so it can report a missing dependency in this state.
The retained revisions and shared blocks remain complete; the test explicitly
constructs and checks this intermediate state.

When the preceding operation is known to have been an interrupted GC, resume its
cleanup with the same store and key:

```text
syrnck gc
syrnck gc --delete
syrnck fsck
```

The resumed GC revalidates every retained closure and file hash before deleting
anything. Missing or corrupt retained objects abort that validation. This
procedure completes an already requested deletion; it does not recover objects
whose retention roots were removed. It is not a general repair command for an
unexplained integrity error. Establish required pins/references before GC, and
retain an archive when removed history must remain available.

Interrupted packing can be retried with `pack` or `pack --compact`. Killed
ingestion may leave complete unreferenced chunks reusable on retry. Interrupted
restoration can be retried if the destination is absent; if already published,
verify and use that complete file rather than overwriting it.

Temporary `.tmp<random-id>` files can remain after a hard kill. They do not enter
the object inventory or retain data. The tests ensure they cannot publish partial
objects or restored files. Automatic removal of all stale temporary files is not
implemented; generated inactive `.syp/.syi` cleanup is a separate mechanism.

## Test isolation and reproduction

Only the private `syrnck_crash_io` object target defines
`SYRNCK_TEST_IO_CHECKPOINTS`. It recompiles the four IO translation units with
the production target's compiler options, definitions and dependencies. The probe
links these objects; the recovery reader links only the normal core. The CLI,
C libraries, installed targets and production core have no checkpoint calls,
labels or runtime fault controls. The suite checks labels in the probe and their
absence from the ordinary shared library and recovery reader. No environment
variable enables these hooks in a normal build.

```text
python scripts/build.py build
python scripts/build.py test -R '^crash_recovery$'
```

`SYRNCK_BUILD_TESTS=OFF` omits the private probe, reader and IO object target.
The suite also works with CLI/network disabled. It uses Python's standard library;
Python never loads the sanitizer-instrumented library itself. Native children
inherit configured sanitizer runtime options.

## Remaining durability work

`native_bisect_crashes` uses the private Store/IO probe to terminate search
start, mark and reset operations at metadata/reference publication boundaries.
Fresh readers validate state/pin pairing, retained file bytes and restartable
results before resuming the operation. Both plain and encrypted Stores are used.

`backup_crashes` separately exercises offline backup creation, restore and
missing-object repair with the same private instrumented IO objects. It kills
writers during chunk/manifest/revision/reference/settings/configuration and
backup-descriptor writes, plus exclusive destination-directory publication.
A fresh production reader preserves crash evidence, verifies every visible
backup/restore and resumes to a complete result. Plain/encrypted fixtures use
different source, backup and restored envelope keys. Unpublished private sibling
directories can remain after process termination and are not automatically
retired. See [backup semantics](BACKUPS.md).

These tests exercise process termination with the OS and filesystem running.
They do not establish survival of power loss, storage-controller write-cache
loss, filesystem corruption, disk exhaustion or failed flush/rename system calls.
Existing exception/publication tests cover separate failure paths, without
exhaustive system-call fault injection.

Initialization interruption, network publication, interrupted multi-file export,
large real-world corpora, stale-temporary reclamation and filesystem-specific
power-loss testing remain further work. Checkpoints do not enumerate every
instruction or concurrent mutation by a process that ignores the store lock.
