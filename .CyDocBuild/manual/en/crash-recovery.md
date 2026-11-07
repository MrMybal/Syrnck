---
id: crash-recovery
title: Interruption and recovery
description: Interruption and recovery
section: Storage
order: 80
parent: storage
revision: 1
---

## Inspect before recovery {#before-recovery}

Open the same Store with its original key and preserve evidence before retrying. Store opening never changes workspace files. For a pending checkout/switch/merge/replay/rebase/reset/stash operation, inspect `syrnck native checkout-status`, then choose `native checkout-recover` to finish or `native checkout-recover --abort` to restore originals before publication. After publication only cleanup may finish. Do not edit private slots or internal roots. See [checkout states and limits](doc:native-checkout).

Missing/corrupt retained data is an error. Use a verified [backup](doc:backups) or [repair copy](doc:repair-copy) for recoverable missing objects, preserving corrupted evidence; GC is not general repair.

## Interrupted garbage collection {#interrupted-garbage-collection}

After a known interrupted GC, dead loose metadata can outlive its dead packed dependencies. Retained closures remain protected, but fsck also audits unreachable metadata and may report those missing dependencies. With the same Store/key, preview `syrnck gc`, inspect retained roots, resume the already requested deletion with `syrnck gc --delete`, then run `syrnck fsck`.

GC revalidates all retained closures/file hashes before deleting; absent/corrupt retained data aborts. This procedure completes cleanup, never recovers versions whose roots were deliberately removed. Do not use it for an unexplained integrity error. Keep required pins/archives and verified backups first.

## Other operations and temporary data {#other-operations}

Retry interrupted packing with pack/compact after correcting the IO issue; active config selects the published generation. Ingestion can leave complete unreferenced chunks reusable on retry. Retry restoration only if the destination is absent; if already published, verify and use it without overwriting. [Directory export](doc:export) and backup publication can leave private sibling staging directories after a kill; they are not automatically retired.

Stale `.tmp<random-id>` files do not enter object inventory or retain data. Automatic global temporary reclamation is not implemented. Inactive generated pack/index cleanup is a separate validated mechanism. Process-termination recovery does not establish power-loss, controller-cache, network-filesystem or hostile-writer guarantees.
