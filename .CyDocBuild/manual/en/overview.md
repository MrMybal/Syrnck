---
id: overview
title: Syrnck
description: Syrnck
section: Syrnck
order: 0
revision: 3
---

## Choose your workflow {#getting-started}

Syrnck is a C++20 block storage and revision engine for large project assets. A file version is described by a complete manifest, so reconstruction never requires replaying a chain of binary patches. Shared chunks can be reused between versions, paths and transfers.

Start with the [walkthrough](doc:usage), then choose [Git assets](doc:git-assets) or [native history](doc:native-history). Inspect [workspace status](doc:native-workspace) before changes and keep a [verified backup](doc:backups) before retiring old versions. The [storage guide](doc:storage) explains what retains those bytes.

## What is available {#workflows}

Git keeps ordinary text/code history, branches, review, merge and other Git features; Syrnck filters store selected assets as pointers. Hosting Git pointers on GitHub/GitLab does not provide a Syrnck block server. A separate block store/server is required for shared asset data.

The standalone engine has commits, frozen staging, branches, tags, recoverable checkout, merge, linear/interactive rebase, cherry-pick, revert, squash, reset, stash, exact rename history and bisect. Each command's guide states its conflict and publication rules. [Transfers](doc:transfers) publish only complete verified object closures. The [resident publication agent](doc:resident-agent) watches local changes and retries native pushes, including after offline edits. Its optional bidirectional mode receives revisions and merges compatible concurrent history. [Permissions and asset leases](doc:server-access) control configured servers.

The [C++/C/Go integrations](doc:integrations) include a regular-file [Syncthing BEP adapter](doc:syncthing). It is not a resident Syncthing daemon. There is no Unreal plugin; the engine stores ordinary asset bytes without interpreting Unreal objects.

## Use this local manual {#reading}

The complete reader works through file:// without a server or Internet connection. Search prose and code with the header or Ctrl+K. Use the navigation tree, keyboard Tab/Enter and arrow keys; compact layouts expose a menu button. Theme follows the system initially and can be changed in the theme menu. Both English and French use the same page and section IDs. An unavailable language route returns to English while keeping the destination.

Bookmarks, notes and reading preferences depend on browser local-storage policy. Page positions are kept during navigation; explicit section links take priority. External reference links are optional and require Internet access only if followed.

Run `syrnck help`, `syrnck help native-tags`, or `syrnck --lang fr help native-tags`. `--section ID` opens a precise section, `--print-url` prints the address without launching a browser, and `--directory PATH` selects another installed Documentation folder. The terminal's `--help` remains the concise command reference. This CLI has no help menu or F1 UI to replace.

## Current boundaries {#limits}

Read the [architecture](doc:architecture), [formats](doc:protocol) and [limits](doc:limits) before embedding or deploying. Encryption protects stored objects, while transfers expose decoded objects to the trusted server. End-to-end encryption, distributed retention and a complete continuous synchronization service are not implemented. Syrnck's original code is MIT; [dependency notices](doc:licenses) apply separately.
