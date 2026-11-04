# Resident Git filters

`syrnck git install` now also configures a repository-local
`filter.syrnck.process` command. A Git operation can send multiple clean/smudge
requests to one process. The CLI opens its Store lazily and retains its bounded
verified-object cache until Git closes stdin. There is no background service.

```text
syrnck git install
git add Content
git checkout another-branch
```

Existing Syrnck-generated single-file filters are retained and the exact earlier
smudge command is upgraded when needed. A different existing clean, smudge or
process command is refused before any filter config is written. For repositories
without an inherited process setting, `git config --local --unset
filter.syrnck.process` restores the single-file filters; `git install` enables the
resident process again. Custom filters require manual integration.

## Behavior and performance scope

The same clean/smudge engine and `local`/`download`/`skip`
[checkout policies](GIT_ASSETS.md#on-demand-checkout-and-pointer-only-clones) apply.
Configuration and environment are read at launch. Skip-smudge needs no Store or
key, even for an entire session; a clean request still requires local storage.
Download creates the remote client only when an object is missing and requires
the existing renewable read leases. Ordinary verified local content stays offline.
Transfer journals, staged pins and complete Git pins keep their usual semantics.

Once opened, the Store keeps its **exclusive process lock for the Git command's
lifetime**, including time waiting for the next request. Other commands against
that Store still fail fast while it is busy. Requests are sequential. Worktrees
share their common on-disk Store, but independent filter processes do not share
a decoded cache. No `delay` capability, parallel checkout scheduler, daemon or
multi-file checkout transaction is introduced.

Keeping a handle avoids repeated Store/index initialization and lets repeated
objects reuse verified decoding. Each access still rereads and fingerprints the
current envelope, so changed/corrupt bytes cannot hide behind the cache. This is
not a throughput claim for Unreal project corpora; those benchmarks remain work.
Full integrity verification before smudge output also remains in place.

## Streaming and bounds

The implementation follows Git's
[filter-process contract](https://git-scm.com/docs/gitattributes#_long_running_filter_process)
and [version negotiation](https://git-scm.com/docs/long-running-process-protocol).
It negotiates version 2 and the offered `clean`/`smudge` capabilities only.
Binary bodies use [pkt-line framing](https://git-scm.com/docs/protocol-common#_pkt_line_format):
65,516 payload bytes per packet, distinct empty-data and flush packets, and no
conversion of content bytes. Text records accept an optional final LF. Metadata
lists are limited to 64 KiB and 256 records; unknown request headers are ignored,
while duplicate keys or missing/unsupported commands/pathnames fail that file.
Pathnames are borrowed context, never paths opened by the protocol adapter.

No response is sent until the request body's terminating flush has arrived.
Normal Syrnck clean consumes the file directly into the ingestion pipeline and
then emits its small pointer. Normal pointer smudge consumes the short pointer,
verifies its closure, and streams the reconstructed asset directly into packets.
Neither path needs a full-asset temporary copy for protocol framing.

A generic handler may write before consuming its input; ordinary raw content
passing through smudge does this. Output is held in at most 64 KiB before spilling
to a private temporary stream. A successful spill is read back in bounded packets
only after input completion. The temporary file closes/deletes with the request,
including on failure. Spill bytes are plaintext and temporary disk usage can grow
with a raw pass-through file; encrypted object storage does not encrypt this
fallback stream. Packet buffers, the Store cache, ingestion pipeline, visited
object keys and metadata add their own memory costs. These are not total RSS or
temporary-disk quotas.

## Failures and shutdown

A file error drains its remaining framed input, reports an English/French CLI
diagnostic on stderr, and returns `status=error`. The next valid request can still
succeed. If content already started, the final status list overrides its earlier
success; Git must reject the file. The installed `filter.syrnck.required=true`
setting makes failures authoritative. No diagnostic is mixed into binary stdout.

Malformed framing, oversized metadata, protocol IO failure or cancellation ends
the process. The CLI exits 130 for cooperative cancellation and 1 for fatal
errors. A normal EOF between requests exits 0, including after a per-file error:
the client reads each framed status rather than treating process exit alone as
the result. Reads blocked on a pipe need data/EOF before cancellation can finish.

The public C++ `git::filter_process` adapter takes unframed handler callbacks; it
has no HTTP dependency. It reports recoverable errors through an optional
diagnostic callback and throws fatal errors. Handlers must honor cancellation,
finish output checks, and use the normal Store/observer serialization contracts.

`git_process_contracts` verifies cache reuse with disk tampering in plain/encrypted
Stores, response gating, bounded spill, late-error framing and cancellation.
`git_filter_process` exercises production CLI conversations, malformed lengths,
truncation/metadata limits, binary and empty packets, error recovery, store-free
skip, custom-config preservation and a real 24-asset Git command using one filter
process. Existing Git checkout/push suites also run with the installed resident
filter. Local validation is Windows x64; other platforms remain CI validation.
