# Persistent native working index

`native status --indexed` opts into a local advisory cache of verified working
file hashes. It avoids rereading unchanged asset contents when the filesystem
can provide the required identity and change metadata. Ordinary `native status`
continues to hash every selected file in full.

```text
syrnck native status --indexed
syrnck native status --indexed --combined --progress-json
syrnck native status --indexed --revision REVISION
syrnck native index-clear
```

The combined form retains the existing branch/index/workspace comparisons,
including stale staging. The cache describes observed working files; it is
separate from the immutable staging index and never stages or commits content.
JSON field names are English in both interface languages.

## Reuse and fallback

Each cached entry records a repository path, size, full BLAKE3 file hash,
executable metadata and a digest of the filesystem identity/change stamp.
Supported files are opened for metadata queries before and after observation.
Reuse requires the identity, modification time, change time, size and mode or
attributes to match the previous verified observation. Recent or future change
and modification times bypass reuse; a two-second guard is deliberately
conservative. This detects ordinary in-place writes that restore both size and
modification time, as well as replacement by another file with matching bytes
or timestamps.

| Platform | Files eligible for reuse |
| --- | --- |
| Windows | Fixed local NTFS/ReFS volumes; 128-bit file ID, volume identity, last-write/change times and attributes |
| Linux | ext-family, XFS, Btrfs and tmpfs; device/inode, nanosecond mtime/ctime and mode |
| macOS | Local APFS; device/inode, nanosecond mtime/ctime and mode |
| Other filesystems/platforms | Full hashing |

UNC/network paths, unrecognized filesystems, failed identity queries and recent
writes use full hashing. Windows metadata handles deny write sharing: an active
writer can defer timestamp updates until close, so its file cannot reuse a
cached hash. A supported identity changing during observation fails the scan
and requires a retry. Existing ignore rules, reparse/symlink rejection and
portable namespace checks still apply.

This is a stat-based acceleration with local cache trust, not an integrity
audit or an atomic filesystem snapshot. Deliberate metadata forgery, clock
manipulation and arbitrary concurrent filesystem replacement are outside its
assumptions. Use ordinary status for an independent byte comparison.
Checkout, reset, stash, commit and staging still use their existing complete
reads and publication fences; they never authorize replacement using this cache.

## Persistence and lifetime

The Store contains one local `working-index.json` pointer, bound to the hash of
the canonical workspace root. Its target is an ordinary content-addressed chunk
containing the index. It uses the Store's existing compression and, for an
encrypted Store, object encryption. The pointer contains only identifiers.
It is replaced atomically after the complete scan and final progress observer.
An observer changing that pointer causes publication to fail instead of
overwriting the change. Cancellation may leave an unreferenced index chunk,
but does not publish a partial pointer.

The cache is disposable. It is not a reference or a version-retention root.
GC can collect its objects; logical backups and replication do not depend on
it. Missing objects, corrupt envelopes and malformed documents trigger a fresh
scan and a new cache generation. Linked/nonregular or oversized pointer files
are rejected rather than overwritten. `index-clear` removes only the local
pointer; ordinary GC can reclaim the remaining chunks.

A damaged old object is not repaired in place. It remains visible to a full
Store audit until ordinary GC collects the unreferenced cache generation.

There is currently one cached workspace per Store. A different canonical root
rebuilds that cache. Each index object is bounded by 16 MiB; if entries exceed
its budget, the scan remains complete and only a subset is cached. Reports
mark `truncated: true`. Snapshots, maps, JSON reports and other allocations are
additional memory; this limit is not a total process RAM bound.

## Reports, C++ and measurement

Indexed reports preserve the ordinary fields and add `working_index`:

- `previous`: `missing`, `ready`, `invalid` or `other_workspace`.
- `hashed_files`, `reused_files`: counts for this observation.
- `supported_files`: files with stable supported identities, including recent
  files that still needed full hashing.
- `cached_files`, `truncated`, `object`: the new bounded cache contents.

`hashed_bytes` counts actual working content bytes read, excluding stored
metadata. A reused file adds zero. Indexed status can write disposable cache
data and emit `publishing` progress; ordinary status remains read-only.

```cpp
#include <syrnck/working_index.hpp>
auto report = syrnck::indexed_native_status(store, project, revision, operation);
auto combined = syrnck::indexed_native_workspace_status(store, project, "main", operation);
auto cleared = syrnck::clear_native_working_index(store, operation);
```

```text
python scripts/benchmark_working_index.py --cli build/Release/syrnck.exe --asset-mib 64 --iterations 5
```

The benchmark uses a synthetic asset, reports subprocess-inclusive durations
and actual content bytes hashed, and checks an equal-size edit with restored
mtime. It is not a real Unreal corpus or cold-cache throughput measurement.

Directory enumeration and ignore evaluation still run on every indexed query,
so additions, removals, renamed paths and ignore changes are observed. The
[resident watcher](NATIVE_WATCH.md) triggers queries from native notifications
and full audits, with bounded hints and overflow recovery. Persistent event
journals and incremental directory enumeration remain separate work. C ABI
and Go bindings do not yet expose this native workspace cache.

Local regression coverage includes plain/encrypted Stores with loose,
single-pack and multi-pack cache reads, persistence across Store lifetimes,
same-size/same-mtime edits, active Windows writers, renames, ignore changes,
staging comparisons, malformed/corrupt/collected caches, cancellation and
pointer publication fences. Forty process termination boundaries cover
initial/replacement cache object and pointer publication; a separate production
reader verifies retained versions and working contents before rebuilding.

Metadata API references: [Windows basic information](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_basic_info),
[Windows final paths](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)
and [Linux stat fields](https://www.man7.org/linux/man-pages/man3/stat.3type.html).
Windows sharing rules: [CreateFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea).
