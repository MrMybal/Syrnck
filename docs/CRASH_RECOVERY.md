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
| Native amendment | Directory chunk/manifest writes and all three modes' revision/reference publication; branch, previous-tip archive and staged-index consumption publish together, preserving parents and retained bytes |
| Native cherry-pick, revert and abort | Candidate revision writes and the checkout journal/prepared-file/move/publication/rollback boundaries; replay creates a child of the old tip and retains old file bytes through resume, abort and GC |
| Native merge and abort | Ordinary and explicitly resolved two-parent candidates, exact fast-forward, the checkout publication/move/rollback boundaries and both-parent retention through recovery and GC |
| Native checkout and abort | Journal/pending roots, prepared files, exclusive backup/install/rollback moves, branch/archive/receipt publication and cleanup; resume or rollback preserves the selected version and leaves no active journal |
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

The local Windows x64 Release run on 5 November 2026 passed **1,280 process-kill
combinations**, plus checkpoint/lock and production-isolation checks. Unix adds
hard-link-boundary cases. This local result does not claim the new suite has
already run on Linux, macOS, ARM64 or sanitizer builds. CTest includes it
automatically in those configured builds.

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

These tests exercise process termination with the OS and filesystem running.
They do not establish survival of power loss, storage-controller write-cache
loss, filesystem corruption, disk exhaustion or failed flush/rename system calls.
Existing exception/publication tests cover separate failure paths, without
exhaustive system-call fault injection.

Initialization interruption, network publication, interrupted multi-file export,
large real-world corpora, stale-temporary reclamation and filesystem-specific
power-loss testing remain further work. Checkpoints do not enumerate every
instruction or concurrent mutation by a process that ignores the store lock.
