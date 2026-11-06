#ifndef SYRNCK_H
#define SYRNCK_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#if defined(_WIN32) && defined(SYRNCK_EXPORTS)
#define SYRNCK_API __declspec(dllexport)
#elif defined(_WIN32) && !defined(SYRNCK_STATIC)
#define SYRNCK_API __declspec(dllimport)
#else
#define SYRNCK_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SyrnckStore SyrnckStore;
typedef struct SyrnckWorkspace SyrnckWorkspace;
typedef struct SyrnckCancellation SyrnckCancellation;
/* ABI 1: 0 = success; 1 = error; 2 = unknown C++ exception;
 * 3 = cooperative cancellation in *_ex calls with operation/cancellation input.
 * Strings are NUL-terminated UTF-8 unless a byte length is specified.
 * Store/workspace handles are not thread safe; serialize calls including close.
 * See the cancellation source's concurrency contract. No input pointer is retained.
 * The binary and this header must come from the same release. */
#define SYRNCK_ABI_VERSION 1u
#define SYRNCK_CANCELLED 3
SYRNCK_API uint32_t syrnck_abi_version(void);
/* Diagnostic text is limited to 4095 bytes, truncated if necessary. */
SYRNCK_API const char *syrnck_last_error(void);
/* Copy the thread-local error without clearing it. required includes NUL.
 * Query with (NULL, 0); insufficient capacity returns 1 without writing. */
SYRNCK_API int syrnck_error_copy(char *out, size_t capacity, size_t *required);
SYRNCK_API int syrnck_store_open(const char *path, bool create, const unsigned char *key, size_t key_len,
                                 SyrnckStore **out);
SYRNCK_API int syrnck_store_close(SyrnckStore *store);
/* Caller provides out_id[65]. Ingested files are retained under ffi/<id>. */
SYRNCK_API int syrnck_ingest_file(SyrnckStore *store, const char *path, char *out_id);
/* algorithm: 0 = FastCDC2020, 1 = fixed (min = average = max).
 * NULL selects the default CDC configuration. */
typedef struct SyrnckChunking {
  uint32_t algorithm, min, average, max;
} SyrnckChunking;
SYRNCK_API int syrnck_ingest_file_ex(SyrnckStore *store, const char *path, const SyrnckChunking *chunking,
                                     char *out_id);
SYRNCK_API int syrnck_restore_file(SyrnckStore *store, const char *id, const char *destination);
/* Verify all chunks, lengths and the complete file_hash without writing a file
 * or changing references. Decoding uses bounded chunk buffers. */
SYRNCK_API int syrnck_file_verify(SyrnckStore *store, const char *id);
/* Exact ranges within manifest size, without truncation. Partial reads verify
 * selected chunks; whole-file ranges also verify file_hash. File output is
 * atomic and exclusive, with no 16 MiB streaming export limit. */
SYRNCK_API int syrnck_restore_file_range(SyrnckStore *store, const char *id, const char *destination,
                                         uint64_t offset, uint64_t length);
/* Buffered ranges are capped at 16 MiB. Query with (NULL,0) validates metadata
 * and bounds, not payloads. Non-NULL output requests verification even for an
 * empty range. Caller bytes stay unchanged on failure. out_length resets to 0
 * on errors except insufficient capacity, which reports required range length. */
SYRNCK_API int syrnck_file_read_range(SyrnckStore *store, const char *id, uint64_t offset, uint64_t length,
                                      unsigned char *out, size_t capacity, size_t *out_length);
/* Metadata-only reuse plan JSON, including NUL. Same query/capacity convention
 * as closure_json. No payload verification or reference changes. */
SYRNCK_API int syrnck_file_compare_json(SyrnckStore *store, const char *source, const char *target, char *out,
                                        size_t capacity, size_t *length);
SYRNCK_API int syrnck_manifest_json(SyrnckStore *store, const char *id, char **out);
/* Free only strings returned by manifest_json or workspace_*_json, once. */
SYRNCK_API int syrnck_string_free(char *value);
/* kind is "chunk", "manifest" or "revision"; IDs are lowercase hex (64 bytes).
 * Reads return verified plaintext; imports verify ID and canonical format.
 * These calls do NOT provide end-to-end encryption over a transport. */
SYRNCK_API int syrnck_object_has(SyrnckStore *store, const char *kind, const char *id, int *present);
/* Query size with (NULL, 0). Otherwise capacity must hold the entire object.
 * length is reset to 0 on errors except insufficient capacity (required size).
 * Maximum object size is 16 MiB. Caller owns all memory. */
SYRNCK_API int syrnck_object_read(SyrnckStore *store, const char *kind, const char *id, unsigned char *out,
                                  size_t capacity, size_t *length);
SYRNCK_API int syrnck_object_import(SyrnckStore *store, const char *kind, const char *id,
                                    const unsigned char *data, size_t length, int *created);
/* JSON array of {kind,id} for the full dependency closure, including root.
 * Query with (NULL,0); length includes NUL, unlike binary object_read. */
SYRNCK_API int syrnck_closure_json(SyrnckStore *store, const char *kind, const char *id, char *out,
                                   size_t capacity, size_t *length);
/* set_ref validates the complete closure and all file hashes before publication.
 * Imported objects remain unpinned until this succeeds. Keep a store open and
 * serialize the entire transfer against GC. Interrupted imports can be resumed. */
SYRNCK_API int syrnck_ref_set(SyrnckStore *store, const char *name, const char *kind, const char *id);
SYRNCK_API int syrnck_ref_remove(SyrnckStore *store, const char *name);
SYRNCK_API int syrnck_store_verify(SyrnckStore *store, size_t *objects);

/* A one-shot cancellation source. request is idempotent and thread safe, even
 * during an operation or a progress callback; the other handles stay serialized.
 * Keep the source alive until every using call AND request has returned. Never
 * close concurrently with request/use. NULL close is accepted; NULL request is
 * an error. A requested source stays requested: create a fresh one to retry. */
SYRNCK_API int syrnck_cancellation_create(SyrnckCancellation **out);
SYRNCK_API int syrnck_cancellation_request(SyrnckCancellation *cancellation);
SYRNCK_API int syrnck_cancellation_close(SyrnckCancellation *cancellation);
typedef enum SyrnckProgressPhase {
  SYRNCK_PHASE_SCANNING = 0, SYRNCK_PHASE_INGESTING = 1,
  SYRNCK_PHASE_RECONSTRUCTING = 2, SYRNCK_PHASE_PUBLISHING = 3,
  SYRNCK_PHASE_VERIFYING = 4, SYRNCK_PHASE_EXPORTING = 5,
  SYRNCK_PHASE_TRANSFERRING = 6, SYRNCK_PHASE_PLANNING = 7, SYRNCK_PHASE_PACKING = 8
} SyrnckProgressPhase;
typedef struct SyrnckProgress {
  uint32_t phase; /* SyrnckProgressPhase */
  const char *path; /* borrowed path bytes; not necessarily NUL terminated */
  size_t path_length;
  uint64_t bytes, total_bytes;
  uint32_t has_total_bytes, complete; /* 0/1 */
  size_t buffered_blocks;
} SyrnckProgress;
/* Synchronous on the calling thread. Borrowed event/path expire on return.
 * Never reenter/close the Store or workspace, throw, or unwind across the ABI.
 * Only cancellation_request may reenter. No callback is retained after return. */
typedef void (*SyrnckProgressCallback)(const SyrnckProgress *event, void *user);
typedef struct SyrnckOperation {
  const SyrnckCancellation *cancellation; /* NULL = no stop request */
  SyrnckProgressCallback progress; /* NULL = no callback */
  void *user;
} SyrnckOperation;

/* Resident advisory workspace observation, ABI 1 additive symbols.
 * Open arms notifications before the first full observation. It holds no Store
 * handle/lock. Both directories must already exist; store_path must be below
 * directory. NULL options selects defaults; supplied limits must be 1..65536.
 * Handles are single-caller; serialize close, wait, status and invalidate. */
typedef struct SyrnckWorkspaceOptions {
  uint32_t polling; /* 0 = native with polling fallback, 1 = polling */
  uint32_t max_paths; /* default 4096; coalesced hints, at most 1 MiB */
  uint32_t max_directories; /* default 65536; native registration budget */
} SyrnckWorkspaceOptions;
SYRNCK_API int syrnck_workspace_open(const char *directory, const char *store_path,
                                     const SyrnckWorkspaceOptions *options, SyrnckWorkspace **out);
SYRNCK_API int syrnck_workspace_open_ex(const char *directory, const char *store_path,
                                        const SyrnckWorkspaceOptions *options,
                                        const SyrnckCancellation *cancellation, SyrnckWorkspace **out);
SYRNCK_API int syrnck_workspace_close(SyrnckWorkspace *workspace);
/* One native wait (0..60000 ms). JSON: version, backend, paths, full_scan,
 * overflow, polling. Hints are retained/coalesced internally until a successful
 * status, even if the caller ignores this output or waits multiple times.
 * Allocated JSON outputs include NUL in length; free with string_free.
 * No size-query call: each success consumes one batch exactly once. On failure
 * *out=NULL, *length=0 (when provided); continuity is invalidated. */
SYRNCK_API int syrnck_workspace_wait_json(SyrnckWorkspace *workspace, uint32_t timeout_ms,
                                          char **out, size_t *length);
SYRNCK_API int syrnck_workspace_wait_json_ex(SyrnckWorkspace *workspace, uint32_t timeout_ms,
                                             const SyrnckCancellation *cancellation, char **out, size_t *length);
/* Uses a caller-opened Store only during this call; close it before idle waits
 * to release its exclusive lock. mode: 0 = branch working status, 1 = revision
 * working status, 2 = combined staging/working status. selector=NULL chooses
 * selected HEAD branch for modes 0/2; mode 1 requires a revision selector.
 * Drains one immediate native batch, then observes all retained hints. The
 * first query and recovery always read all working bytes. Failure invalidates
 * the inventory; the next successful query is full. Output <=128 MiB + NUL.
 * *_ex propagates cooperative cancellation to waits, registration and hashing;
 * status *_ex additionally reports synchronous progress. OS IO/JSON encoding
 * already underway may finish before the next check. Cancellation returns 3,
 * resets output and invalidates continuity; a late request may follow success.
 * Disposable cache writes can complete before cancellation; no refs are moved.
 * Native hints are advisory; schedule independent full audits with invalidate.
 * This API does not journal checkpoints or transfer/commit working files. */
SYRNCK_API int syrnck_workspace_status_json(SyrnckWorkspace *workspace, SyrnckStore *store,
                                            const char *selector, uint32_t mode, char **out, size_t *length);
SYRNCK_API int syrnck_workspace_status_json_ex(SyrnckWorkspace *workspace, SyrnckStore *store,
                                               const char *selector, uint32_t mode,
                                               const SyrnckOperation *operation, char **out, size_t *length);
SYRNCK_API int syrnck_workspace_invalidate(SyrnckWorkspace *workspace);
#ifdef __cplusplus
}
#endif
#endif
