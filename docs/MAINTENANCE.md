# Maintenance progress and cancellation

`gc`, `gc --delete`, `pack` and `pack --compact` support `--progress` and
`--progress-json` on stderr, along with cooperative Ctrl+C. Their ordinary final
JSON reports on stdout are unchanged. In C++, use
`Store::gc(remove, OperationOptions)` or `Store::pack(PackOptions, OperationOptions)`.

Preparation is cancellable: retained-file validation, physical inventory
planning, loose-key sorting, pack construction and verified readback check the
stop token. An observer exception also aborts preparation. The old catalog,
references and existing source files remain intact. Temporary files are removed
on cooperative unwind. Completed immutable packs prepared before cancellation
can remain inactive; the next successful maintenance cleanup reclaims generated
inactive pack/index files. Opening the store does not select them automatically.

Phases identify the counters' scope. `verifying` counts decoded payload reads.
`planning` counts physical envelope bytes inspected for the maintenance plan.
`packing` counts verified source envelopes supplied to the new pack writer;
it is not a durable-write counter and excludes pack headers/indexes. The packing
total is known after planning. GC may alternate packing and verification as it
prepares each partially live pack. A `publishing` event is the final callback and
cancellation check before committing maintenance. Phase completion describes
preparation; only successful return/exit confirms the entire operation finished.

Once the catalog has been published, maintenance finishes loose-object and
obsolete-pack cleanup without further observer callbacks or cancellation checks.
If no catalog change is needed, the same boundary precedes the first destructive
deletion. This prevents a cooperative stop from stranding dead loose metadata
after its packed dependencies have been removed. A stop that arrives after that
boundary can therefore finish with exit 0. The CLI may appear quiet during this
final cleanup. Dry-run GC never enters destructive cleanup and remains
cancellable throughout its work.

Ordinary IO failures and hard termination can still interrupt committed cleanup.
The existing [crash recovery procedure](CRASH_RECOVERY.md) applies: retained roots
stay complete, and a known interrupted GC may need its cleanup resumed before a
full audit of unreachable metadata succeeds. These options do not change object
formats, physical pack versions, retention rules or exclusive-store locking.

`maintenance_operations` tests cancellation in each preparation phase and in
readback, observer failures, unchanged source bytes, complete retained-file
reconstruction and retry. Private test checkpoints also request cancellation
exactly after catalog publication and after the first loose deletion, proving
that committed cleanup finishes. Tests run in plaintext and encrypted stores.
The checkpoint implementation is not linked into production binaries.

Native IO, codec calls and index validation already in progress may finish before
the next stop check. Existing memory costs remain: one mark bit per packed
object for GC, sorting of loose-only keys for packing and separate metadata and
traversal allocations. Progress/cancellation add constant-size counters and
callbacks rather than another full object inventory.
