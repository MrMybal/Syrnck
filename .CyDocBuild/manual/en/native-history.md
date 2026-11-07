---
id: native-history
title: Native revision ancestry
description: Native revision ancestry guide.
section: Version control
order: 300
revision: 1
---

# Native revision ancestry {#native-revision-ancestry}

`native ancestry` examines the complete revision graph reachable from two
snapshots. It reports both ancestry directions and all best common ancestors.
It does not choose an arbitrary base when a criss-cross merge has several.

```text
syrnck native ancestry tags/v1 HEAD --progress-json
syrnck native ancestry heads/main heads/feature --max-revisions 100000 --max-links 1000000
```

The JSON result contains `source`, `target`, `source_is_ancestor`,
`target_is_ancestor`, `related`, sorted `merge_bases`, `visited_revisions` and
`parent_links`. A revision is its own ancestor. Unrelated histories return an
empty base list and `related: false`, with a successful command exit status.
The CLI accepts the existing native revision selectors. The C++ API takes IDs:

```cpp
auto report = store.native_ancestry(left_id, right_id, syrnck::NativeGraphLimits{}, operation);
```

Traversal is iterative, with linear graph work after revision metadata decoding.
Each distinct revision is loaded once. All parent edges, including merge parents,
participate; duplicate parent entries count toward the link budget. Defaults are
100,000 revisions and 1,000,000 links. Explicit limits allow 1..1,000,000 revisions
and 0..10,000,000 links. Exceeding a budget fails without a partial answer.
The base-list size is also bounded conservatively below a 16 MiB JSON report.
Memory scales with visited revisions and parent links; revision metadata itself
remains subject to the existing 16 MiB object limit.

Only revision objects are read. File manifests and block payloads are not needed,
so this query does not establish file integrity. Missing or corrupt revision
metadata anywhere in the reachable graph is an error, even if a likely answer
was found earlier. There is no shallow-history fallback or implicit network fetch.
Use `fsck` or a verified snapshot operation for file integrity.

Queries never change refs, stored files or working files. Planning progress,
cooperative cancellation and English/French diagnostics follow the existing
operation contract. Callbacks must not reenter the Store. C ABI and Go exposure
of native history operations remain future work.
