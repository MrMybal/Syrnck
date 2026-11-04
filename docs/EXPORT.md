# Transactional native exports

`native export REVISION NEW_DIRECTORY` validates the revision and its complete
dependency closure, reconstructs every file into a new sibling staging directory,
and publishes the directory with one exclusive rename. The destination is absent
until the complete tree is ready. Existing files, symlinks and directories,
including an empty directory created concurrently, are never replaced.

```text
syrnck native export REVISION restored-project --progress
syrnck native export REVISION restored-project --progress-json
```

In C++, `Store::export_revision(id, destination, OperationOptions)` accepts the
same stop token and synchronous observer used by reconstruction. `verifying`
events cover the initial closure; `exporting` events report aggregate restored
bytes and a known total for the current snapshot, with the current relative
file path when available. Empty files are included. A completed exporting event
means preparation finished. The subsequent `publishing` event is the last
cancellable callback before the directory rename. Successful return/exit is the
publication result. References and source objects are unchanged.

The staging directory is named `.syrnck-export-<32 lowercase hex digits>` in the
destination's existing parent. This keeps staging and publication on the same
filesystem. Unix staging roots use owner-only permissions, retained by the
published root. Files contain their ordinary decoded bytes; executable metadata
is restored on Unix.
Cancellation, callback exceptions and ordinary write/publication failures clean
up only this invocation's staging directory. Output paths remain validated
revision paths, reconstructed from complete manifests.

The implementation uses exclusive rename primitives: Windows
[MoveFileExW without replacement](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw),
Linux [renameat2 with RENAME_NOREPLACE](https://man7.org/linux/man-pages/man2/rename.2.html),
and macOS [renamex_np with RENAME_EXCL](https://developer.apple.com/documentation/foundation/urlresourcevalues/volumesupportsexclusiverenaming).
If the OS or filesystem does not support exclusive directory rename, export
fails and removes its staging directory. There is no fallback that could
overwrite an empty destination. Files are individually flushed; Unix directory
entries are flushed before publication and the parent is flushed afterwards.
The final rename is a commit point: cancellation arriving after it starts can
be too late. A final durability error can also leave a complete published tree.
These operations do not establish power-loss guarantees for every filesystem.

Hard termination can leave a staging directory, while the destination is either
absent or complete. The normal reader does not automatically remove such
directories: confirm ownership and whether their contents are still needed
before removing them. Retry to a missing destination builds a fresh staging
tree. No staged file is treated as stored revision data or as a retention root.

Native tests cover plaintext/encrypted, loose/packed stores, cancellation during
verification and reconstruction, final publication cancellation, observer/write
failures, concurrent empty destinations and executable modes. The
[process-kill suite](CRASH_RECOVERY.md) also stops exports during file writes and
directory publication, then verifies and resumes with the production reader.
Local validation is Windows x64; the Linux/macOS branches await the next CI run.

Reconstruction streams file blocks, but the initial public `closure` still
materializes its object-key set, and revision/manifest JSON keeps the existing
size limits. This is a complete-directory export to a new location. It does not
yet implement checkout into an existing working tree or conflict resolution.
