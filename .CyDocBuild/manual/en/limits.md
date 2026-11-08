---
id: limits
title: Current limits and planned work
description: Current limits and planned work
section: Syrnck
order: 900
revision: 15
---

## Workspace and versioning {#workspace}

[Named and detached HEAD](doc:native-detached) are supported. [Linked native workspaces](doc:native-workspaces) retain separate HEADs/indexes and portable backup snapshots. Partial materialization, recoverable selection changes, hard reset and stash with matching selections are available. [Archived unregistration and restoration](doc:native-workspace-archives) preserve captured dirty work, complete HEAD/index trees and context roots. [Linked relocation](doc:native-workspace-relocation) preserves the whole directory on one filesystem, with resume/abort and case changes; cross-filesystem and primary/Store moves remain work to complete. The persistent working index and observer avoid some repeated hashing but do not produce an atomic filesystem snapshot or a complete event history. Native OS events may overflow; conservative rescans and polling remain necessary.

Exact-content renames are recognized, while edited renames/copies, their merge/rebase integration and recursive synthesis of multiple merge bases remain incomplete. Legacy v1 revisions have no author or signature fields. Optional [v2 authorship](doc:native-authorship) records authors/committers and Ed25519 signatures; [server policies](doc:server-signatures) can authorize keys and identities. Registration is manual and policy changes require restart; OS signing-key vaults and distributed revocation remain incomplete. Read each rebase/merge mode's constraints; available commands are not universal Git compatibility.

## Team operation and continuous synchronization {#sync}

Configured servers provide users/roles, branch checks and expiring asset leases. Native history transfer and Git catalog publication remain separate from a distributed multi-server transaction. The [resident publication agent](doc:resident-agent) captures local edits, deletions and exact renames, retains offline snapshots and retries guarded native pushes. The optional bidirectional mode applies incoming descendants through recoverable checkout and merges compatible concurrent histories. Unresolved conflicts, acknowledged remote deletions/rewinds and rewritten local history are blocked. The agent resumes only its own guarded checkout/merge transactions, including offline recovery of prepared signed targets without a private signing key. Manual or unrelated recovery stays explicit. The explicit [conflict-copy policy](doc:resident-agent#conflict-copies) preserves concurrent file contents; namespace collisions still block. OS service installation remains unfinished.

The Syncthing adapter handles regular-file block indexes/requests/responses. Database integration, discovery, device pairing, version vectors, conflict copies and complete folder synchronization belong to the embedding application or future work.

## Security, retention and recovery {#security}

Objects can be encrypted at rest, with separately keyed backups/restores. Trusted servers receive plaintext objects over the secured transport. End-to-end encryption, password-based keys, system key vaults, distributed rotation/revocation, automatic replica repair and distributed GC remain separate work.

Retention is explicit. Archives preserve versions until their protecting roots are retired; a verified backup can protect manual retirement, but automatic age/release policies are not implemented. Process interruption and power-loss durability are different guarantees. No operation establishes universal power-loss, network-filesystem or hostile external-writer safety.
