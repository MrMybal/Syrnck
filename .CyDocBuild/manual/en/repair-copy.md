---
id: repair-copy
title: Rebuild a damaged Store in a verified new location
description: Rebuild a damaged Store in a verified new location guide.
section: Storage
order: 490
parent: storage
revision: 1
---

# Rebuild a damaged Store in a verified new location {#rebuild-a-damaged-store-in-a-verified-new-location}

Repair copies combine the current Store's retained roots with a fully verified
backup. Intact current objects come from the current Store; missing or corrupt
objects come from the backup by their exact kind/ID. The output is a new,
verified normal Store. Both inputs remain unchanged.

```text
syrnck --store damaged backup repair-copy-plan verified-backup
syrnck --store damaged backup repair-copy verified-backup recovered
syrnck --store recovered fsck
syrnck --store recovered native export HEAD recovered-files
```

The destination must not exist, its parent must exist, and it must be outside
both inputs. An existing directory is refused even if empty. Select the new
Store explicitly for inspection and use; the command does not replace or
rename the damaged source.

## Preserve current history while recovering old bytes {#preserve-current-history-while-recovering-old-bytes}

An older backup does not need to contain all current objects. For example,
when an old retained asset block is corrupt but a newer code revision is still
intact locally, the copy recovers that block from backup and preserves the
newer revision from the source. Current heads, frozen staging snapshots,
archives, tags and other portable roots are retained together.

Preflight traverses that combined graph, validates object identities and hashes
every reconstructed file before creating output. A required object unavailable
or invalid in both inputs blocks creation. The entire backup is verified,
including its descriptor, settings, retained closure and inventory, even if
only one of its objects is needed. `repair-copy-plan` performs those checks
without writing files or references and reports `ready`, `missing`, `corrupt`,
counts, the source references hash and donor snapshot ID.

Copying freshly imports the verified logical objects using the output key,
copies saved ingestion settings and publishes the selected reference table.
A complete output fsck verifies its object inventory and reconstructed files.
Only then does an exclusive directory rename publish the new destination.
The final observer runs before source reference/settings and donor descriptor
checks, so a callback changing either input cannot publish a stale result.

`workspace/HEAD` and completed checkout receipts are omitted as in
[backup restore](doc:backups), since their absolute paths bind another checkout.
Object retention roots remain intact. The result has no immutable `backup.json`
descriptor and can accept future revisions normally. Current working files,
untracked/ignored data and Git repository metadata are not copied as filesystem
files; any retained native versions remain exportable from their revisions.

## Corrupt loose objects and packed envelopes {#corrupt-loose-objects-and-packed-envelopes}

The read-through applies to chunks, manifests, revisions and opaque retained
metadata objects, from loose files or indexed pack envelopes. A corrupt
envelope can therefore be recovered into fresh storage without rewriting,
deleting or quarantining the original pack. Logical IDs remain the same;
physical compression, packing and encryption representations can differ.

The source must still open with its valid key, configuration, reference table,
pack catalog/index, expected pack lengths and headers. Rebuilding damaged or
missing pack indexes/headers, damaged config/ref tables and live in-place
replacement are separate recovery operations. The command copies retained
logical objects; it does not salvage unreachable unknown bytes or create an
identical physical pack layout.

The original [missing-object repair](doc:backups#repair-missing-retained-dependencies)
keeps its stricter behavior: `backup repair --apply` imports missing objects
in place, while any corrupt existing object blocks that operation. Use a
repair copy when the retained current graph can be recovered from both inputs.

## Encryption and operation boundaries {#encryption-and-operation-boundaries}

`SYRNCK_KEY` opens the damaged source; `SYRNCK_BACKUP_KEY` opens the donor.
For this command, `SYRNCK_RECOVERY_KEY` optionally selects a third independent
output key. With no recovery key, the CLI keeps the primary key, or the donor
key when only the donor is encrypted. An encrypted input requires an output
encryption key; the API never silently publishes plaintext from protected input.
No command prints key values.

Source and donor Stores are held under their normal exclusive locks and must
be quiescent: no active checkout, incomplete transfer or unexpired active
read/upload lease. This is an offline operation. It is not replication,
automatic failover, an encrypted transport or end-to-end encryption.

Cancellation and cooperative errors before directory publication remove the
private staging destination. Hard process termination can leave an unpublished
private sibling, while the named destination remains absent or a complete,
verified Store. Retry with an absent destination, or inspect an already
published complete result. The two inputs and their corrupt bytes remain
available for independent inspection.

The APIs in `syrnck/backup.hpp` are available in core-only builds:

```cpp
auto plan = syrnck::plan_repaired_copy(source, backup, operation);
auto result = syrnck::create_repaired_copy(source, backup, new_directory,
                                           destination_key, operation);
```

Reports are limited to 16 MiB. Object identity sets and traversal metadata
remain in memory; payloads are copied and verified one bounded CAS object at
a time. Full preflight/copy/output verification deliberately reread retained
data. This is a verified recovery path, without a production throughput claim.
