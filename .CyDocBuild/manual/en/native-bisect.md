---
id: native-bisect
title: Persistent native bisection without checkout
description: Persistent native bisection without checkout guide.
section: Version control
order: 210
parent: native-history
revision: 1
---

# Persistent native bisection without checkout {#persistent-native-bisection-without-checkout}

`syrnck::NativeBisect` narrows a known good/bad range through a revision DAG,
including merges. Its state is persistent and each mark uses an expected state
ID. The search does not move branches, alter staging or change working files.
Testing a candidate remains an explicit caller action.

```text
syrnck native bisect-start GOOD_REVISION BAD_REVISION
syrnck native bisect-status
syrnck native export CANDIDATE_REVISION /new/candidate-directory
syrnck native bisect-good CANDIDATE_REVISION --expect-state STATE_ID
syrnck native bisect-bad CANDIDATE_REVISION --expect-state STATE_ID
syrnck native bisect-skip CANDIDATE_REVISION --expect-state STATE_ID
syrnck native bisect-reset --expect-state STATE_ID
```

Start requires distinct revisions with the known good revision reachable from
the initial bad revision. Normal native revision selectors work in the CLI.
The JSON report contains `session`, `state`, `status`, `candidate_count`,
`next_revision`, `first_bad`, `good`, `skipped` and the current `bad` revision.
Use the exact suggested revision and `state` from the latest report when
recording a test result. Another update makes an earlier state token invalid.
A new random session identity prevents a stale token from becoming valid when
the same range is searched again.

Known good revisions exclude their complete ancestry. The remaining candidates
are ancestors of the current bad revision. For each eligible test revision,
the engine counts how many candidates remain if it is bad and how many remain
if it is good, selecting the most balanced split with a deterministic ID tie
break. Bitset ancestry propagation handles merges without counting shared
ancestors twice. The current known bad revision is not offered as another test.

`status: found` identifies one `first_bad` revision. A skipped/unbuildable
candidate remains a possible introduction point. If no unskipped test remains,
the report is `ambiguous`, `first_bad` stays null and `possible_first_bad` lists
the remaining possibilities. It does not silently choose one skipped commit.
As with a normal binary regression search, outcomes should describe the same
test with a defect that persists through descendants; contradictory or changing
test criteria cannot establish a reliable first introduction.

## State, retention and recovery {#state-retention-and-recovery}

One active search is supported per Store. `bisect/active` points to a strict
canonical version-1 chunk descriptor; `bisect/pin` retains the initial bad
revision and all its ancestor/file dependencies. Starting publishes state and
pin together, after complete file verification. Removing or moving a visible
branch during the search does not allow GC to collect its test history.

Marks publish a new immutable state object through an expected-reference
transaction. Old state objects can be collected; there is no growing snapshot
archive on every mark. Cancellation/crash before reference publication leaves
the previous active state; complete orphan metadata can remain. Fresh processes
read the same search and continue with `bisect-status`. No runtime cache is
required for correctness. Corrupt descriptors, missing/mismatched pins,
out-of-history results and contradictory good/bad metadata are rejected.

Reset ends an unfinished or finished search, moving its history protection to
`archive/bisect/INITIAL_BAD` and retaining its final descriptor under
`bisect/receipts/SESSION`. These changes publish together. The old file versions
remain reconstructable after GC. `native archives --kind bisect` inspects the
history roots; [backup-protected retirement](doc:archive-retention) can later
remove them explicitly. A new search is then independent of the old session.

The revision/metadata traversal is bounded at 100000 revisions, 1000000 parent
links and a 64 MiB estimated graph-metadata payload budget. At most 4096 unresolved
candidate revisions are admitted. The dense ancestry bitsets use at most 2 MiB;
container/cache/allocator memory is additional. Narrow an overly large initial
range with a later known good revision. There is no persistent commit-graph
index yet, so repeated status calls can reread revision metadata.


Interactive checkout, automatic command execution, multiple simultaneous
workspace searches, blame, rename following and commit signatures remain
separate features.
