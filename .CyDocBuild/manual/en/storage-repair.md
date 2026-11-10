---
id: storage-repair
title: Repair corrupted storage in place
description: Recover missing or damaged object copies while preserving roots and unaffected packs.
section: Storage
parent: backups
order: 51
revision: 1
---

## Preview and apply {#commands}

Use a separately [verified backup](doc:backups) as the donor. Stop normal writers and open both Stores with their respective keys. The primary Store accepts the usual raw, protected-file or [vault provider](doc:system-vaults#providers); `SYRNCK_BACKUP_KEY` opens an independently encrypted donor.

```text
syrnck --store damaged backup repair-storage-plan verified-backup
syrnck --store damaged backup repair-storage verified-backup --expect-plan PLAN_ID
syrnck --store damaged fsck
```

Use the exact `plan` ID from the preview. Applying recomputes the full audit and refuses stale state before writing. The plan binds the source path, references, settings, configuration, active object-copy bytes and verified donor snapshot. An added unreferenced object or changed ciphertext also invalidates the preview. There is no implicit apply mode.

Both Stores must be quiescent: no active checkout recovery, incomplete transfer or unexpired read/upload lease. An immutable backup cannot be the repair target. Object and pack directories must be unlinked; object files must be ordinary nonlinked files.

## Coverage and preservation {#coverage}

The audit checks the complete retained graph and all inventoried objects, including unreferenced objects and both loose/packed copies of the same ID. It reconstructs full file hashes through a verified read-through view. A healthy local alternate copy takes precedence over the donor, so an older backup need not contain intact newer local data. Missing or damaged required data unavailable from both inputs blocks the operation before mutation. The donor is fully verified, rather than trusting filenames or sizes.

Repair imports missing logical objects, atomically replaces damaged loose envelopes and rebuilds only packs containing damaged payloads. Healthy envelopes in a rebuilt pack are copied without recompression; all entries are retained, including unreferenced objects. New packs are reread and verified before one atomic catalog update. Other active packs, roots, branches, staging, archives, settings, unrelated inactive packs and unknown files are preserved. The source's existing encryption mode/key and logical object IDs remain unchanged; the donor is never modified.

Once the new catalog is published, only the superseded affected pack/index files are retired. Repair does not run GC. Archived versions remain reconstructible even when a history rewrite removed them from current parent chains.

Reports include `missing_count`, `corrupt_count`, damaged-copy counts, `packs_to_rewrite` and the bound `plan`. The `missing`/`corrupt` arrays show at most 1,000 IDs each; `lists_truncated` explicitly indicates omitted display rows. Counts and the physical commitment still cover the full audit. Success adds `applied`, `imported_objects`, `replaced_loose_copies` and `rewritten_packs`.

## Interruption and recovery {#interruption}

The entire repair is not one transaction. Each missing-object import, loose replacement and final pack-catalog switch publishes complete verified bytes atomically. Cancellation or a process failure can leave a verified subset repaired while other objects remain damaged. Obtain a **fresh** preview and apply its new ID to resume. Completed repairs are not undone, and roots are not rewritten.

New unpublished packs or temporary files can remain after interruption. A failure after catalog publication can also leave superseded inactive pack files. A later ordinary pack/GC cleanup can retire inactive generated files; the repair itself does not sweep unrelated files. An IO, output-allocation or cancellation error can follow completed repairs: inspect the new plan and `fsck` to establish the actual outcome before retrying. Final verification is cancellable; published bytes remain valid.

## C++ API {#api}

```cpp
#include <syrnck/backup.hpp>

auto preview = syrnck::plan_storage_repair(source, verified_backup, options);
auto result = syrnck::repair_storage(
    source, verified_backup, preview["plan"].get<std::string>(), options);
```

The APIs use existing exclusive Store handles. Serialize access and do not reenter them from progress callbacks. Progress/stop behavior follows [operation controls](doc:operations). Auditing uses bitmaps for packed logical objects, sorted loose stamps and sets of damaged/missing identities. Payload working buffers are bounded per object; loose identity/stamp metadata and the traversal stack still scale with the inventory. This is a full offline integrity audit, not an incremental background scrubber or remote replication protocol.

## Boundaries and qualification {#limits}

The source must still open with a valid configuration, reference table, pack catalog/index, pack sizes and pack headers. Repair covers object-envelope corruption and missing logical dependencies. It does not reconstruct lost indexes/configuration, salvage unknown unreachable bytes, rotate distributed keys or establish automatic failover. A [verified repair copy](doc:repair-copy) or [backup restoration](doc:backups) remains appropriate for a fresh destination. The older `backup repair --apply` command keeps its strict missing-only behavior.

Tests cover plaintext/encrypted loose/v2/v3 layouts, independent donor keys, archives after parent rewrites, newer staged data, shadowed duplicate copies, oversized envelopes, stale plans, donor/root changes and cancellation after partial progress. Dedicated writer processes are terminated at every traced IO boundary; a separate production reader verifies read-only recoverability and resumes from a fresh plan. These are process-interruption tests, not a universal power-loss or hostile external-writer guarantee.
