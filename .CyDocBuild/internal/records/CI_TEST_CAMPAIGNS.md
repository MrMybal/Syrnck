# Bounded CI campaigns with complete scenario coverage

The Windows job for `109db8b` (Actions run `42600120993`, job
`129102210335`) ended with nine deadlines exceeded. These were aggregate
workspace, checkout edit, bisect, stash and audit tests, plus two checkout-edit
and two partial-stash process-interruption partitions. The job summary did not
report an assertion failure for those tests. Completion messages in two timed
out processes are insufficient to identify whether cleanup or other scenarios
were still running.

## Changes

`tests/test_cases.hpp` partitions complete, named C++ scenarios into contiguous
nonempty groups. It does not partition assertions or change the operations
under test. Each successful case reports verification and cleanup separately,
then removes its own directory after native handles have closed. The existing
owned-child deletion guard remains in force.

| Suite | Complete scenarios | CTest groups | Existing deadline per group |
| --- | ---: | ---: | ---: |
| Native workspace archives | 27 | 4 | 180 s |
| Active checkout edit preservation | 120 | 8 | 120 s |
| Native bisect | 9 | 9 | 90 s |
| Native stash | 31 | 4 | 120 s |
| Streaming audit | 15 | 4 | 90 s |

The 4,098-revision bisect limit case keeps all real durable writes. Its CTest
`PROCESSORS=4` reservation isolates that fixture from other tests in the CI
parallel budget of four. No engine flush, object verification, interruption
checkpoint or scenario was disabled.

Python checkout-edit interruptions use eight partitions for eight scenarios;
partial-stash interruptions use twelve for twelve. All observed durable
boundaries are still terminated and verified. A successful copied case is
retired immediately after read-only validation and recovery assertions instead
of leaving hundreds of copies for one final cleanup. Ordinary stash
interruptions retain their four partitions and share the same cleanup helper.
Auxiliary checkout namespace/race/collision tests keep their scenario-based
assignment.

Local process campaigns exposed Windows error 145 while deleting already
verified case directories. Python cleanup now applies the same two-second
bound as the existing C++ fixture helper for Windows errors 32/145 only, and
rechecks the owned physical child before every attempt. Access denial, other
errors and expiration still fail the test. This retry applies only to disposable
test copies, never to an engine write, recovery operation or assertion.

Tests retain the system temporary directory. A build-volume placement probe
was faster for the bisect fixture, but broad placement changes are not required
for this scenario-partition change.

## Audit and diagnosis

`test_case_plan` captures CTest's actual command declarations during the build.
`test_case_partitions` checks that every one of the 202 C++ scenarios and all
20 crash scenarios appears exactly once in those commands, with no disabled,
inverted or skip-return-code declarations. The cached plan includes the build
configuration and declaration-file SHA-256; stale declarations fail the audit.

Do not invoke nested `ctest --show-only` from a running CTest test. It rewrites
`Testing/Temporary/LastTest.log`, even though no tests execute. Plan generation
therefore runs before test execution. Avoid building another configuration or
running another CTest against the same build directory during a campaign.

The workflow retains `Testing/Temporary` and generated case plans even on
failure. Flushed JSON timing records distinguish scenario verification from
cleanup and show the last active case at a deadline.

Failed stash reader/probe commands also preserve their synthetic fixture under
`.scratch/stash-crash-failures`, included in diagnostic artifacts. A local
build-volume probe encountered both cleanup error 145 and strict native checkout
inventory refusals, including a disappearing `new/3.BIN.tmp` entry. Those
engine refusals were not ignored or declared fixed. The fixture copy makes a
recurrence inspectable; final qualification retains normal temporary placement.

`checkout_edit_checkpoint_paths` covers a separate confirmed probe issue:
Windows short fixture paths were compared lexically against canonical Store
paths, misclassifying chunk/manifest/revision checkpoints as filesystem events.
The probe now canonicalizes its owned root before binding and classification.
Four plain/encrypted primary/partial cases compare the complete checkpoint
sequence through canonical and alternate path spellings. Windows requests real
8.3 spellings when available; other filesystems also exercise a lexical alias.
The local Windows regression exercised four real short-path aliases. This
changes probe coordinates, not native engine operations or crash boundaries.

## Local measurements before final platform qualification

Windows x64, Release, parallel four, same development machine:

- Original five aggregate suites: all passed, total wall time 134.83 s;
  streaming audit took 82.34 s of its 90 s budget.
- New 29 scenario groups plus selector and declaration audit: 31/31 passed,
  total wall time 107.37 s using the system temporary directory. Scenario
  cleanup accounted for 8.436 s summed across concurrent processes.
- The unchanged 4,098-revision case took 49.19 s with competing groups,
  20.50 s alone on the system temporary volume, and 13.60 s alone on the
  build volume. In the last run, durable fixture construction took 12.680 s,
  limit validation 0.375 s, and cleanup 0.491 s.

These are test-fixture measurements, not an Unreal-project throughput claim.
They do not establish hosted Windows, sanitizer, Linux or macOS qualification
of this harness change; the actual workflow results must be checked.

## Reproduce

```text
python scripts/build.py build
python scripts/build.py test -R "^(test_case_selection|fixture_lifetime|crash_shard_selection|test_case_partitions)$" --output-on-failure
python scripts/build.py test -L case-shard --parallel 4 --output-on-failure
python scripts/build.py test -R "^(checkout_edit_crashes_|native_partial_stash_crashes_|native_stash_crashes_)" --parallel 4 --output-on-failure
```

For Windows Debug, build with `--config Debug`, then test with `-C Debug`.
Preserve `LastTest.log` before a subsequent plan build when comparing runs.
