# Retry policy and shared traffic targets

The HTTP agent retries transient failures and can pace object traffic in either
direction. The same options apply to push and pull:

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --requests 4 --retries 3 --upload-kib-s 4096
syrnck agent pull https://blocks.example.invalid asset received --requests 4 --download-kib-s 8192 --progress-json
```

These commands require a configured server and `SYRNCK_TOKEN`. Use `--ca` for a
private certificate authority. The CLI supports English/French; JSON fields
remain canonical English. No credential is persisted by this policy.

## Retry boundaries

`--retries` accepts **0..8 additional attempts**, default **3**. Zero disables
application retries. Each request has its own budget. Retriable operations are
`GET v1/refs`, `POST v1/missing` (a presence query), and object `GET`/`PUT`.
Object PUT is idempotent because the content is addressed by its verified hash.
A lost upload acknowledgement can therefore cause the same complete object to
be sent again. Downloads restart the individual object, not a byte range;
partial responses never enter the Store.

| Failure | Policy |
| --- | --- |
| HTTP 408, 429, 500, 502, 503, 504 | Retry within the request's remaining budget |
| DNS resolution, connection, send/receive, empty reply, partial response, request timeout | Retry within the remaining budget |
| Other HTTP statuses, including 401/403/404/409 and 501 | Fail immediately |
| TLS verification, malformed protocol, response-size rejection, invalid object or dependency, observer exception, cancellation | Fail immediately |
| Final reference PUT, including a transient status or lost reply | **Never retry automatically** |

The initial delay is **250 ms**, doubled for each additional attempt and capped
at **30 seconds**. A valid `Retry-After` integer or HTTP-date can increase the
delay up to that same cap; malformed fields are ignored. Waiting uses a monotonic
clock after converting an HTTP-date. There is currently no randomized jitter.
Authentication and payload validation errors are not masked by retries.

Object requests waiting for another attempt keep their request slot and charged
upload/response buffers. They detach from libcurl's multi handle while waiting.
Other active requests can continue; a retry cannot bypass the configured buffer
or request budget. The original upload vector and reserved response capacity
are reused. Cancellation wakes both retry and traffic waits. Progress observers
remain on the coordinator thread, and request counters reset on each attempt.

The final JSON adds `object_retries`, counting additional **object** attempts
scheduled by this policy. Control-plane retries are excluded. Existing
`transferred_objects` and `transferred_bytes` still count successful logical
objects once; they are not wire-traffic counters and exclude repeated or partial
attempts. Exhaustion returns an error without final success statistics.

The [publication boundary](TRANSFERS.md#publication-boundary) is unchanged:
after the final remote CAS is dispatched it completes without observer calls or
cooperative cancellation. Ambiguous publication exits **3** and requires checking
the remote reference. A retry of the whole command obtains a fresh expected
reference when starting a fresh session. An unfinished session preserves its
original expectation and uses [journal reconciliation](TRANSFER_SESSIONS.md)
instead of replaying an ambiguous compare-and-set.

## Traffic targets

`--upload-kib-s` and `--download-kib-s` specify **KiB/s (1,024 bytes/s)**, with
**0 meaning unlimited**. Both accept 0..1,073,741,824 KiB/s. The defaults are
unlimited. They describe aggregate **object-body** average targets for the whole
operation, shared across parallel requests, retries and push batches. They are
not multiplied by `--requests`.

Each active handle receives a share of the target using libcurl's
[send](https://curl.se/libcurl/c/CURLOPT_MAX_SEND_SPEED_LARGE.html) and
[receive](https://curl.se/libcurl/c/CURLOPT_MAX_RECV_SPEED_LARGE.html) limits.
The coordinator also accounts libcurl's upload/download body counters across
attempts and paces further work against a common elapsed-time budget. It drains
that budget before returning to publication, so many small objects cannot each
claim a fresh startup allowance. A push keeps the same budget across its missing
object batches. Retry delays, local work and network idle time count as elapsed
time; actual throughput may be lower than the configured target.

These are software average targets, **not an instantaneous packet policer**.
Short bursts, socket buffers and libcurl scheduling are allowed. HTTP headers,
TLS framing, reference/presence requests and the final publication are outside
the target. Observed object-body bytes from failed/repeated attempts and object
acknowledgements are included. A cancelled or failed operation can end before
paying off its last burst; no future session inherits its pacing budget.

Object request deadlines include the worst-case time for a maximum-sized object
at the configured target and concurrency, plus the ordinary 120-second allowance.
Connection establishment still has a 15-second timeout. Control requests and
the final CAS retain their 120-second timeout. Limits do not establish a total
operation deadline, and server/proxy timeouts still apply.

In C++, `net::TransferOptions` exposes `max_retries`, `retry_delay_ms` (1..30,000),
`upload_bytes_per_second` and `download_bytes_per_second`. Nonzero rates must be
at least `max_requests` bytes/s and at most 1 TiB/s. Pass the options after
`OperationOptions` to `push`/`pull`, or as the second argument to `refs`. The
independent `refs` call supports retries but has no object traffic to limit.

## Recovery and validation

Restarting the command still negotiates remote presence or checks local verified
objects and reuses completed imports. [Persistent sessions](TRANSFER_SESSIONS.md)
save the requested root and reference expectation and block local GC until
completion or deliberate discard. Retry budgets and pacing credit are not
persisted. [Server upload leases](SERVER_UPLOAD_LEASES.md) protect uploads while
renewed. [Server read leases](SERVER_READ_LEASES.md) protect exact pull roots until
their objects are local; background resume remains future work.

`resilient_transfers` runs a real HTTP peer that returns transient statuses,
loses acknowledgements and truncates downloads. It exercises control/object
retries, exhaustion, explicit disable, Retry-After, exponential delay, encrypted
imports, bounded buffers, cancellation during waits, aggregate pacing over small
objects, and single dispatch of uncertain reference publication. Pacing assertions
measure controlled fixture durations, not production throughput. Validation here
is Windows x64; other platform and sanitizer runs still require CI.
