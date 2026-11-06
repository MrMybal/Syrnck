# Server identities, asset locks and native history transfer

Start one server per repository. The configured access mode authenticates each
request as a principal, checks write permissions for repository paths and branch
names, and enforces asset leases when publishing a revision. Raw object reads
are repository-wide: these path rules restrict writes, not confidentiality.
Use separate repositories when users must not read each other's objects.

## Configure identities

Generate a separate random bearer token for each person or device with
`syrnck keygen`. Keep tokens in a secret manager or protected environment.
Set `SYRNCK_TOKEN` locally and run `syrnck token-hash` to obtain its BLAKE3
digest without opening a Store. Do not place raw tokens in the configuration,
command history, logs or Git. Tokens should have at least 32 random characters.

An access file has this exact version-1 schema. Replace each placeholder with
the 64 lowercase hexadecimal characters returned by `token-hash`:

```json
{
  "version": 1,
  "principals": [
    {
      "id": "alice",
      "token_hash": "REPLACE_WITH_ALICE_TOKEN_HASH",
      "role": "writer",
      "write_paths": ["Content", "Source"],
      "branches": ["main"]
    },
    {
      "id": "reviewer",
      "token_hash": "REPLACE_WITH_REVIEWER_TOKEN_HASH",
      "role": "reader",
      "write_paths": [],
      "branches": []
    }
  ],
  "branches": [
    {"name": "main", "fast_forward": true, "lock_paths": ["Content"]}
  ]
}
```

```text
syrnck --store server-store serve --access access.json --require-lease
syrnck locks identity --remote https://blocks.example.test/
```

The existing HTTPS proxy requirement remains. `--access` replaces the shared
token authentication; `SYRNCK_TOKEN` on the server is not a fallback. The file
is validated at startup, and changes require a restart. Removing a principal
revokes future requests; replacing its digest rotates its token while keeping
its identity and lock ownership. Ongoing requests complete under the startup
configuration. This is bearer-token authentication, not end-to-end encryption.

Roles are `reader`, `writer` and `admin`. All authenticated principals may read
objects, inspect configured branch references and acquire read-transfer leases.
Writers/admins may upload objects, but may publish only the explicitly listed
branches and allowed paths. An admin has the additional right to prune expired
asset leases; it does not bypass branch or path rules. Readers must have empty
write-path and branch lists. An empty string in `write_paths` means the whole
repository; an empty list grants no writes. A path prefix matches itself and
descendants at `/` boundaries, using portable Unicode normalization/case folding.
`Content` does not grant access to `Contentious`.

IDs and branch names are single reference components, at most 128 bytes.
IDs are unique after portable case folding; token hashes are unique. Access
configuration is capped at 1 MiB, 256 principals and 1024 branch rules. Lists
are capped at 1024 entries; individual path prefixes at 4096 UTF-8 bytes.
Treat the configuration as an administrator-controlled file.

## Use remote asset leases

```text
syrnck locks acquire-plan --remote https://blocks.example.test/ Content/Map.uasset Content/Map.ubulk
syrnck locks acquire --remote https://blocks.example.test/ --seconds 3600 Content/Map.uasset Content/Map.ubulk
syrnck locks list --remote https://blocks.example.test/ --limit 100
syrnck locks renew --remote https://blocks.example.test/ --fence 1 Content/Map.uasset Content/Map.ubulk
syrnck locks release --remote https://blocks.example.test/ --fence 1 Content/Map.uasset Content/Map.ubulk
syrnck locks prune --remote https://blocks.example.test/
```

Omit `--owner`: the server supplies the authenticated ID. Owner fields in HTTP
requests are rejected. Every requested path must be writable by that identity.
Grouped operations use the same atomic expiry/fencing semantics as the
[local lease table](ASSET_LOCKS.md). Plans are read-only and do not reserve
paths. A reader can list leases; only an admin can preview/remove expired
records remotely. Local administrative commands remain available while the
server is stopped and its Store lock is released.

Store the `path` and `fence` fields from acquisition in a JSON array for push:

```json
[{"path":"Content/Map.uasset","fence":1},{"path":"Content/Map.ubulk","fence":1}]
```

```text
syrnck native push https://blocks.example.test/ --lock-claims claims.json --require-lease
```

The server validates the authenticated owner, exact current generation and
expiration of every claim. It requires a claim for every changed path under a
branch's `lock_paths`, including deletion and both sides of a case change or
rename. An active exact lease is honored even outside mandatory prefixes.
Another writer cannot use a copied fence to publish an asset. The earliest
expiry is checked again after full dependency/file verification, immediately
before the reference transaction. Expiry or an observed backward wall-clock
change rejects publication. Callers must keep long-running locks renewed.

The server mutex and exclusive Store lock serialize operations. Upload/read
transfer lease IDs are internally scoped to the authenticated principal; knowing
another principal's external lease ID does not expose its receipt or fencing
token. Stable principal IDs preserve transfer resumption across token rotation.

## Publish and fetch autonomous history

```text
syrnck native push https://blocks.example.test/ --branch main --remote-branch main
syrnck native fetch https://blocks.example.test/ --remote-branch main --remote-name origin --require-read-lease
syrnck native ancestry HEAD remotes/origin/main
syrnck native merge-plan remotes/origin/main
```

Push transfers the complete dependency closure of the selected local branch,
including parent history and all asset blocks. It requires the server identity
extension. A configured server accepts only revision roots on configured branch
names. Expected-reference compare-and-swap protects concurrent publication;
`fast_forward: true` additionally refuses a replacement that does not descend
from the existing head. Uploaded objects may remain unreferenced on rejection,
but the branch does not move.

Write checks cover every newly reachable commit compared with every parent,
plus changes between the published and proposed heads. Adding a forbidden path
in an intermediate commit and removing it before push still fails. Merge
publication is deliberately conservative: differences against either parent
need write permission and applicable leases. Publication currently admits at
most 100000 new/previous revisions per traversal, 1000000 parent links and
16384 changed portable paths within a 16 MiB path budget. Lock claims are capped
at 1024; large protected changes must be divided into smaller publications.

Fetch downloads and verifies the selected remote revision into an immutable
`native-fetch/REVISION` root, then atomically updates
`remotes/REMOTE_NAME/BRANCH`. It archives the previous tracking head under
`archive/fetch/REVISION`. The selected branch, staging index and working files
are untouched. Merge/rebase remain explicit, recoverable local operations.
Tracking references are valid explicit revision selectors. Native transfer CLI
names are capped at 64 bytes to fit local reference limits.

Transfers reuse existing resumable sessions, read/upload leases, bounded object
parallelism, compression and cancellation. `agent resume` can complete an
interrupted fetch's object transfer; rerun `native fetch` to finish tracking
publication. Retained fetch roots and archives currently require explicit
administrative retirement; GC alone does not remove them.

## HTTP extension

`GET /v1/identity` returns `version`, `id` and `role`. Capabilities advertise
`asset_locks: 1` and `branch_policies: 1` in configured access mode. Legacy
shared-token mode reports zero for both, with a null identity, and refuses
remote asset-lock operations. Existing shared-token transfer clients remain
supported in legacy mode.

All lock routes use `POST /v1/locks/ACTION`, with JSON bodies capped at 1 MiB:

| Action | Exact request fields |
| --- | --- |
| `list` | `after` (path or null), `limit` (1..1000) |
| `acquire-plan`, `acquire` | `paths` (1..1024 strings), `seconds` (1..86400) |
| `renew` | `claims` (path/fence array), `seconds` |
| `release` | `claims` |
| `prune` | `delete` (boolean; false previews) |

`PUT /v1/refs/NAME` accepts `root`, `expected`, optional `upload` and optional
`locks` (the claim array). It enforces the same rules for direct requests and
CLI requests. Configured reference listing exposes only configured branch
names, not local indexes, archives, lock metadata roots or workspace paths.
401 denotes invalid authentication, 403 denied permissions/policies/claims,
409 a competing reference update, 412 an invalid upload lease and 428 a missing
required upload lease. Malformed requests and failed lock operations return
400; missing objects/receipts return 404. Successful plans can contain conflicts
without changing the Store. Mutating requests are not automatically retried.

No Syncthing interoperability, service-resident watcher, commit signatures,
distributed GC, per-path read isolation or remote audit log is implied by this
extension. Those remain separate development work.
