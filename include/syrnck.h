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
/* ABI 1: 0 = success; 1 = error; 2 = unknown C++ exception.
 * All strings are NUL-terminated UTF-8. Handles are not thread safe.
 * Serialize calls including close. No input pointer is retained.
 * The binary and this header must come from the same release. */
#define SYRNCK_ABI_VERSION 1u
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
SYRNCK_API int syrnck_manifest_json(SyrnckStore *store, const char *id, char **out);
/* Free only strings returned by syrnck_manifest_json, exactly once. */
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
#ifdef __cplusplus
}
#endif
#endif
