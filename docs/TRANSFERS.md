# Transfer progress, cancellation and publication

The one-shot HTTP agent supports the same `OperationOptions` as local operations.
`Remote::refs`, `push` and `pull` accept a stop token and a synchronous observer.
CLI usage keeps the ordinary final statistics on stdout:

```text
syrnck agent push https://blocks.example.invalid files/MANIFEST asset --progress
syrnck agent pull https://blocks.example.invalid asset received --progress-json
```

Authentication still uses `SYRNCK_TOKEN`; encrypted local stores use their own
`SYRNCK_KEY`. These examples need an actual configured service. URL, token and
key material are never included in progress events. Events identify the relative
protocol path, such as `v1/objects/chunk/ID`.

`transferring` bytes are HTTP request-body plus response-body bytes for one
attempt. They reset between requests and retry attempts. The total may initially be unknown; it
becomes known when libcurl learns a content length, and is exact at successful
request completion. Control requests also produce events. A completed request
does not mean an imported object has passed validation or a reference has been
published. The existing final `transferred_bytes` statistic counts only logical
payloads of newly transferred objects; it excludes control messages and reused
objects. Text is localized and JSON keys/phases are canonical English.

The client uses libcurl's
[transfer-progress callback](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html)
to abort ordinary requests while they are in progress, including idle waits.
Checks also run between requests, objects, local validation and reference writes.
Callbacks run on the calling thread. Exceptions are captured inside C callbacks,
abort the request and are rethrown in C++ after libcurl returns. They never unwind
through libcurl. Native IO/codecs and resolver work already running may finish
before the next cancellation check; cancellation is cooperative, not an immediate
system-wide timeout.

Push validates its complete source closure and full file hashes before sending
object payloads. It negotiates missing objects in batches, then uploads only
those objects. Pull verifies each received object before local import. An
interrupted transfer can leave reusable unreferenced objects. Retrying negotiates
or checks their presence again; incomplete responses are not imported. A pulled
manifest may temporarily lack some dependencies until retry finishes, so a full
audit of unreachable objects can fail during this incomplete state. Previously
retained roots remain reconstructible. An unfinished local session blocks GC
until resumed or deliberately discarded. See [persistent sessions](TRANSFER_SESSIONS.md).

## Publication boundary

Pull checks cancellation throughout the final local closure validation and
atomic reference write. Before that write starts, cancellation preserves the old
reference. Push emits a `publishing` event and checks cancellation immediately
before preparing its final remote compare-and-set request. A stop from that
callback prevents dispatch and leaves the remote reference unchanged. The
publishing journal write also checks cancellation before atomic replacement.

Once the publishing marker commits, the client dispatches and finishes the CAS
even if a stop arrives. A crash between marker and dispatch is conservatively
treated as an uncertain publication. Aborting a dispatched CAS cannot undo a
server-side publication. There are no
observer callbacks after this commit point. A successful response completes the
push normally; a late Ctrl+C can therefore finish with exit 0. This final request
retains its existing 120-second timeout. The server publishes only after checking
the full dependency closure. A conflict (409) is a normal rejected publication.

If the CAS response is lost, or its HTTP status does not unambiguously reject
publication, C++ throws `net::PublicationUncertain`. The CLI emits a localized
diagnostic, leaves stdout empty and exits **3**, even if an interrupt was also
requested. The server may already have updated the reference. Inspect that
reference before deciding whether to retry. Transport failures, server errors
and unexpected responses are treated conservatively; 401/403/404/409 remain
ordinary request failures. Resume uses the persistent journal to confirm a
matching remote root; it does not automatically repeat an ambiguous CAS.

Ordinary cooperative cancellation exits **130**. It does not remove already
verified objects and does not delete existing references. Object formats,
authentication rules and the v1 HTTP protocol are unchanged.

## Validation and remaining work

The HTTP regression fixture exercises real libcurl requests, pre-cancellation,
mid-request stops, callback exceptions, cancellation before publication, resume,
encrypted local imports, full source-hash rejection, compare-and-set conflicts,
late cancellation and a server that commits then drops its response or sends 500.
A real Windows Ctrl+Break interrupts an idle HTTP request; Unix SIGINT is included
for the next CI run. Existing workflows also exercise the production server and
packed/encrypted stores. Local validation is Windows x64.

Object requests default to sequential execution and can run concurrently with
`--requests` and a buffer budget. See [parallel transfers and resource limits](PARALLEL_TRANSFERS.md).
Transient idempotent requests now support bounded retries with backoff, and
object traffic can use shared average targets. See the [retry and traffic policy](TRANSFER_POLICY.md).
Persistent sessions support explicit resume, saved reference expectations and
local GC holds; see [journal guarantees and limits](TRANSFER_SESSIONS.md).
Push still materializes its closure and sorted object list; pull retains a seen
set and pending traversal. Background watching, distributed transfer leases and
automatic Git push/checkout integration remain future work.
