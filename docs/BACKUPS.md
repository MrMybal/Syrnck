# Verified offline backups, restore and missing-object repair

The C++ API in `syrnck/backup.hpp` creates logical Store snapshots. A backup
contains every object reachable from its retained roots, including parent
revisions, archived heads, tags, staging snapshots and the asset-lock fencing
counter. Complete file hashes are verified before a destination becomes visible.
The source remains unchanged.

```text
syrnck --store project/.syrnck backup create /existing-backup-parent/snapshot-001
syrnck --store /existing-backup-parent/snapshot-001 backup verify
syrnck --store /existing-backup-parent/snapshot-001 backup restore /existing-parent/recovered-store
syrnck --store /existing-parent/recovered-store fsck
syrnck --store /existing-parent/recovered-store native export heads/main /existing-parent/recovered-files
```

The destination must be a new directory with an existing parent, outside the
source Store. An existing destination is never replaced, even if empty. Create
and restore write into a private sibling directory, flush it, then publish it
with an exclusive directory rename. Cancellation or an exception before that
rename exposes no partial destination. A hard process termination can leave an
unpublished `.syrnck-export-*` sibling; it is not a valid backup until published.
Rerunning the command creates a new private staging directory. Automatic
retirement of those abandoned directories remains future work.

Stop the server before opening its Store for backup. The exclusive Store lock
prevents simultaneous normal writers. Active checkouts, incomplete transfer
sessions and unexpired active read/upload leases block backup creation. Complete
or explicitly retire those operations first. Asset leases and their fencing
counter are retained, with their original expiry times.

These are backups of retained storage, not filesystem images. Working files,
uncommitted edits outside staging, ignored/untracked files, Git metadata,
unreferenced objects, transfer journals and private checkout directories are
not copied. Absolute `workspace/HEAD` bindings and completed checkout receipts
are explicitly omitted; their retained source/target revision roots remain.
Restore produces an ordinary independent Store. Export a chosen revision to
recover its files. Rebinding/recreating a working workspace is a separate step.

## Verify and change encryption keys

Each backup has a canonical `backup.json` descriptor recording hashes of its
reference table and settings, the object count, decoded byte count, creation
time and omitted references. `backup verify` checks this descriptor, all
retained dependencies, every reconstructed file hash and the complete object
inventory. Changing backup references/settings or adding an unreferenced object
invalidates verification. The descriptor is an integrity receipt, not a signed
or externally authenticated root. Keep backups protected from modification.

Primary Store encryption uses `SYRNCK_KEY`. For create/restore, optional
`SYRNCK_BACKUP_KEY` selects the destination key. If omitted, an encrypted source
keeps the current key; a plaintext source produces a plaintext destination.
The C++ API requires a destination key for encrypted sources and refuses a
silent plaintext downgrade. Object IDs remain stable when envelopes are
re-encrypted. Copying to a new key does not revoke old backups or hide equality
between matching object IDs.

To verify a backup, put its own key in `SYRNCK_KEY`, because it is the primary
Store for that command. To restore with another key, use the backup key in
`SYRNCK_KEY` and the new key in `SYRNCK_BACKUP_KEY`. Keys are 32 bytes encoded as
64 hexadecimal characters. Store keys securely outside Git and backup paths.
The backup descriptor/reference table is not encrypted; object envelopes use
the existing at-rest encryption. This is not end-to-end encryption, a password
KDF, an OS key vault or live distributed key rotation.

Restoration fully verifies the backup before creating a destination. It imports
decoded objects into fresh envelopes, retains the reference table/settings and
verifies the resulting Store. The restored Store does not keep `backup.json`,
so normal future revisions do not leave a misleading immutable-backup receipt.
Keep the original backup until restored revisions have been exported and checked.

## Repair missing retained dependencies

```text
syrnck --store damaged-store backup repair snapshot-001
syrnck --store damaged-store backup repair snapshot-001 --apply
syrnck --store damaged-store fsck
```

For an encrypted repair source, `SYRNCK_BACKUP_KEY` is its key. `SYRNCK_KEY`
continues to open the damaged destination. Repair does not assume both keys are
equal; an unset secondary key means the repair source is plaintext.

The default command is a read-only plan with `ready`, `missing`, `corrupt` and
`references_hash`. A blocked plan exits 1 with JSON. Applying a blocked plan
fails without a partial repair. Other invalid commands emit a diagnostic.

The backup is fully verified first. The planner traverses the destination's
retained portable roots, reading local objects and using backup objects only
where a dependency is absent. It verifies complete reconstructed file hashes
through this combined view before any write. A backup can therefore repair an
older asset while newer complete local revisions remain untouched. It need not
contain those newer revisions. A required object absent from both Stores blocks
the operation before writes.

`--apply` imports only missing verified objects. Each import publishes a complete
envelope atomically; cancellation/crash can leave a verified subset repaired.
Rerun the plan/apply operation to finish. Existing roots and objects are not
removed or replaced, and a completed repair is idempotent. Plan output is capped
at 16 MiB. The traversal currently keeps object identities in memory, as the
existing closure API does; it is not a disk-backed inventory.

Corrupt existing objects block the whole repair. They are preserved for
inspection. This first repair API does not replace corrupt loose objects,
rebuild lost/corrupt packs or repair a corrupt reference table/configuration.
Restore into a new Store when existing storage cannot be safely repaired.

## Validation and limits

Native tests cover plain/encrypted sources on loose/single-pack/multi-pack
storage, destination key changes, retention after restore and GC, source
immutability, destination collisions, cancellation and repair with newer local
history. CLI tests cover independent encryption keys, bilingual output,
descriptor tampering and no-write repair planning. A dedicated process-kill
campaign stops writers at object/reference/descriptor writes and directory
publication, then audits and resumes in a fresh process.

The initial validation platform is Windows x64. These checks exercise process
termination with a running OS, not power loss or failed flushes. Full-store
verification deliberately reads retained data repeatedly; production corpus
measurements and disk-backed traversal remain necessary. Live replication,
multi-server failover, scheduled scrubbing, distributed retention and automatic
replacement of corrupt data remain separate work.
