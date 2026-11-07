---
id: transfers
title: Transfer progress, cancellation and publication
description: Transfer progress, cancellation and publication guide.
section: Transfers
order: 400
revision: 1
---

# Transfer progress, cancellation and publication {#transfer-progress-cancellation-and-publication}

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
request completion. Foreground control requests also produce events; background
lease renewals have no user callbacks. A completed request
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
retained roots remain reconstructible. An unfinished local session retains its
source or available downloaded objects during GC until completed or deliberately
discarded. See [persistent sessions](doc:transfer-sessions).
Negotiated [read leases](doc:server-read-leases) protect the exact source root while
missing objects are downloaded. They are released before local publication,
when the verified objects and unfinished local journal protect reconstruction.

## Server authentication and request bodies {#server-authentication-and-request-bodies}

The server consumes the HTTP request body before checking credentials, with the
existing **16 MiB** payload limit and 30-second read timeout. It then requires
exactly one `Authorization: Bearer <token>` header before application JSON parsing,
object decompression, Store access or publication. Missing, incorrect and duplicate
credentials receive `401` with `WWW-Authenticate: Bearer realm="Syrnck"`, including
requests to unknown routes. Fully receiving the body avoids a Windows TCP reset
that could otherwise hide the rejection response when the server closes the
connection with unread data.

This applies to well-formed, fully received bodies with either `Content-Length`
or chunked framing. HTTP framing, unsupported transport encoding and payload-size
errors can precede authentication and keep their transport error status. A
provisional `100 Continue` permits sending the body; it does not authorize access.
The server closes connections after HTTP errors. Body reception still occupies
a server worker; deployment quotas and admission controls remain future work.

## Publication boundary {#publication-boundary}

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
matching remote root; it does not automatically repeat an ambiguous CAS. On
compatible servers, a [published upload receipt](doc:server-upload-leases) can
also complete recovery after a later writer changed the reference. A leased
412/428 precondition rejection leaves the session available for deliberate retry.

Ordinary cooperative cancellation exits **130**. It does not remove already
verified objects and does not delete existing references. Object formats,
authentication rules and the base v1 HTTP protocol remain compatible; upload
leases use an explicitly negotiated extension.

## Validation and remaining work {#validation-and-remaining-work}


Object requests default to sequential execution and can run concurrently with
`--requests` and a buffer budget. See [parallel transfers and resource limits](doc:parallel-transfers).
Transient idempotent requests now support bounded retries with backoff, and
object traffic can use shared average targets. See the [retry and traffic policy](doc:transfer-policy).
Persistent sessions support explicit resume, saved reference expectations and
local GC holds; see [journal guarantees and limits](doc:transfer-sessions).
Push still materializes its closure and sorted object list; pull retains a seen
set and pending traversal. A complete resident sync service and distributed retention remain future work. [Git push/checkout integration](doc:git-assets) is available through catalogs, policies and opt-in hooks.
