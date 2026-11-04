# Bounded parallel object transfers

The agent can overlap HTTP object requests using libcurl's
[multi interface](https://curl.se/libcurl/c/libcurl-multi.html). Store reads,
imports, dependency discovery and observer callbacks remain on the calling
coordinator thread. No Store handle is shared with HTTP worker threads.

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --requests 4 --transfer-buffer-mib 64 --progress
syrnck agent pull https://blocks.example.invalid asset received --requests 4 --transfer-buffer-mib 64 --progress-json
```

These examples require an actual configured server and the existing token/key
environment. The default is **one object request**, with a **64 MiB buffer
budget**. `--requests` accepts 1..32 and `--transfer-buffer-mib` accepts 17..1024.
In C++, pass `net::TransferOptions` as the fifth argument of `push` or `pull`,
after `OperationOptions`. Existing calls remain valid and run one object request
at a time by default.

Push negotiates missing keys in batches of at most 4,096, validates its source
closure and prepares upload payloads on the coordinator. Pull discovers
dependencies as verified metadata arrives. A seen set prevents duplicate
downloads, including dependencies already scheduled in another active request.
Locally available pull objects use one verified `get` rather than a separate
presence read followed by another payload read. Imported objects are validated
before dependency traversal or reference publication.

## Limits and counters

The scheduler charges actual upload-vector capacity plus a reserved response
buffer for every outstanding object request. Downloads reserve the maximum
logical object size, **16 MiB**, each. Upload acknowledgements are limited to
**4 KiB** and reserve that much response space. Larger replies fail the object
request. Before decoding another upload, the scheduler also requires enough
free budget for a maximum object and its acknowledgement. Consequently actual
concurrency can be lower than the requested maximum.

Object response vectors reserve their permitted capacity once on first data,
avoiding old-and-new growth buffers during object transfer. Credits remain
charged while the coordinator consumes a completed response. Completed requests
are removed from the multi handle before consumption; other requests remain
active. Idle waits use a bounded
[multi poll](https://curl.se/libcurl/c/curl_multi_poll.html). The session can reuse
connections across object requests in the same batch/walk.

The final statistics retain `transferred_objects`, `transferred_bytes` and
`reused_objects`, and add:

| Field | Meaning |
| --- | --- |
| `peak_object_requests` | Maximum simultaneous scheduled object requests |
| `peak_object_buffer_bytes` | Maximum charged upload/response-buffer credits |
| `object_retries` | Additional object attempts scheduled after transient failures |

The buffer counter includes reserved response capacity, even when a response is
small. It is **not process RSS**. It excludes allocator overhead, libcurl/TLS
internal buffers, the Store's caches, codec scratch, closure/seen/pending keys,
metadata and sequential control-plane requests. A fully reused transfer can
report zero object requests while still negotiating and publishing a reference.
The options do not bound all metadata or total process memory.

## Failure and publication

An exhausted/non-retriable request failure, corrupt response, observer exception or cooperative stop
removes remaining easy handles and releases their payloads. Removal happens
outside libcurl callbacks. C callbacks capture exceptions and return an error;
the coordinator rethrows the original exception. Previously retained roots stay
complete; already verified unreferenced objects can be reused on retry.

Transient failures retry with bounded backoff; waiting requests retain their
slot and buffer credits while other active requests can continue. Optional
upload/download targets are shared across parallel requests. See the
[retry and traffic policy](TRANSFER_POLICY.md) for the allowlist, counters,
units, pacing limits and cancellation rules.

Progress events from different protocol paths can interleave. Each request's
byte counter remains scoped to its own path. Completion of a request does not
publish a reference. Once every needed object is present, the existing
[publication commit point](TRANSFERS.md#publication-boundary) still applies:
local reference writes check cancellation, and the final remote CAS finishes
once dispatched. An ambiguous CAS result still throws `PublicationUncertain`
and makes the CLI exit 3. There is no automatic CAS retry.

`parallel_transfers` uses a real HTTP peer with controlled latency to verify
overlap, request limits, byte-budget throttling, duplicate suppression,
encrypted local imports, cancellation, callback exceptions, corruption,
oversized upload replies and byte-identical reconstruction after retry. The
fixture delay is a concurrency test, not a production throughput measurement.
The existing workflows also cover the production HTTP server and packed stores.
Local validation is Windows x64; other platform/sanitizer runs await CI.

The production server still serializes Store operations under its mutex while
HTTP IO can overlap. Metadata traversal and imports remain coordinated rather
than parallel. [Persistent journals](TRANSFER_SESSIONS.md) retain local recovery
state and block local GC for unfinished sessions. Network compression,
distributed transfer leases and multi-source scheduling remain future work.
