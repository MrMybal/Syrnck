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
/**
 * @doc api-c-abi
 * @locale en
 * @title C ABI: ownership and errors
 * @section API
 * @symbol syrnck_store_open
 * Include `<syrnck.h>` and use the matching `syrnck_ffi` library; define SYRNCK_STATIC for static linking.
 *
 * ## Handles and buffers {#ownership}
 * Open with syrnck_store_open and close exactly once. Serialize calls on each handle, including close.
 * Workspace sessions retain no Store; open it only for queries. Paths and strings are UTF-8. Buffers belong to the caller unless a
 * function explicitly allocates a string; release those strings with syrnck_string_free.
 * IDs need 65 bytes including NUL. Size-query conventions differ between JSON strings and binary
 * object reads; follow the comments beside each declaration in the distributed header.
 *
 * ## Status and diagnostics {#errors}
 * ABI 1 returns 0 on success, 1 on ordinary error, 2 on an unknown exception and 3 on cooperative
 * cancellation for operation-enabled calls. No C++ object or exception crosses the boundary.
 * Copy syrnck_last_error or use syrnck_error_copy on the same OS thread immediately after failure.
 * Diagnostics are canonical English and limited to 4095 bytes. Do not free the thread-local pointer.
 *
 * ## Prepare and publish catalogs {#catalogs}
 * syrnck_object_put creates an unpinned chunk or canonical native metadata object. C++ computes
 * the typed ID and rejects duplicate JSON fields. syrnck_ingest_buffer_ex (<=16 MiB) and
 * syrnck_prepare_file_ex (streamed) prepare unpinned manifests without temporary plaintext.
 * Publish a root before releasing the Store to GC; legacy ingest_file_ex still pins under ffi/ID.
 * syrnck_refs_json reads the typed reference table. syrnck_refs_compare_json compares a complete
 * group of name/expected/value entries and verifies all replacement closures before one publication.
 * A false comparison is not permission to retarget an old plan. On nonzero status the reset applied
 * output proves nothing: an IO error can follow atomic publication, so reload and establish its outcome.
 * See [catalog transactions](doc:go-binding#catalog-transactions) for ownership and an example.
 *
 * ## Guarded native workspace actions {#native-actions}
 * workspace_native_state_json_ex reads target, typed revision, materialization and checkout state.
 * Capture and checkout require the exact SyrnckNativeExpected target/revision/policy object.
 * Capture retains local working changes; plan is read-only; checkout_owned uses the existing native
 * journal and refuses dirty work. recover_owned requires the exact persisted owner and candidate.
 * checkout_status_json_ex reads an exact core journal/receipt without applying it. Completed receipts
 * prove past application with owner/source/target evidence; subsequent local edits remain separate.
 * A session is bound to one Store path, registered context and working directory. Mutations
 * invalidate observation continuity. On error reload state: publication may already have completed.
 * See [native actions](doc:workspace-abi#native-workspace-actions) for recovery and locking contracts.
 *
 * ## Observation and cancellation {#operations}
 * Cancellation handles are one-shot and may be requested from another thread or progress callback.
 * The owning application must serialize destruction against users. A callback borrows its strings
 * only for its duration. Read [workspace sessions](doc:workspace-abi), [Go](doc:go-binding), and
 * [verified ranges](doc:file-ranges) for complete API contracts and examples.
 */
/**
 * @doc api-c-abi
 * @locale fr
 * @title ABI C : propriété et erreurs
 * @section API
 * @symbol syrnck_store_open
 * Inclure `<syrnck.h>` et utiliser la bibliothèque `syrnck_ffi` correspondante ; définir SYRNCK_STATIC pour le lien statique.
 *
 * ## Handles et tampons {#ownership}
 * Ouvrir avec syrnck_store_open puis fermer une seule fois. Sérialiser les appels et la fermeture de chaque handle.
 * La session de workspace ne garde aucun Store ; l'ouvrir pour les requêtes. Chemins et chaînes sont UTF-8. Les tampons appartiennent
 * à l'appelant, sauf allocation explicite d'une chaîne à libérer avec syrnck_string_free.
 * Un ID demande 65 octets, NUL inclus. Les requêtes de taille diffèrent pour le JSON et les objets
 * binaires : respecter les commentaires de chaque déclaration dans l'en-tête distribué.
 *
 * ## Codes de retour et diagnostics {#errors}
 * L'ABI 1 rend 0 pour le succès, 1 pour une erreur ordinaire, 2 pour une exception inconnue et 3 pour
 * une annulation coopérative dans les appels acceptant une opération. Aucun objet ni exception C++
 * ne traverse l'interface. Copier syrnck_last_error ou appeler syrnck_error_copy immédiatement après
 * l'erreur, sur le même thread OS. Les messages anglais canoniques sont limités à 4095 octets.
 * Ne pas libérer le pointeur local au thread.
 *
 * ## Préparer et publier les catalogues {#catalogs}
 * syrnck_object_put crée un bloc ou objet de métadonnées natif canonique sans référence. C++ calcule
 * l'ID typé et refuse les champs JSON en double. syrnck_ingest_buffer_ex (<=16 Mio) et
 * syrnck_prepare_file_ex (flux) préparent des manifests sans référence ni fichier temporaire en clair.
 * Publier une racine avant de libérer le Store au GC ; ingest_file_ex garde son pin ancien ffi/ID.
 * syrnck_refs_json lit la table typée. syrnck_refs_compare_json compare un groupe complet de lignes
 * name/expected/value et vérifie les fermetures avant une seule publication. Une comparaison échouée
 * n'autorise pas à recibler un ancien plan. Sur statut non nul, applied remis à zéro ne prouve rien :
 * une erreur IO peut suivre la publication atomique ; relire et établir son résultat avant de réessayer.
 * Voir les [transactions de catalogue](doc:go-binding#catalog-transactions) pour les règles et l'exemple.
 *
 * ## Actions natives de workspace protégées {#native-actions}
 * workspace_native_state_json_ex lit cible, révision typée, matérialisation et état de checkout.
 * Capture et checkout exigent la cible/révision/objet de sélection exacts dans SyrnckNativeExpected.
 * Capture conserve le travail local ; plan est consultatif ; checkout_owned utilise le journal
 * natif et refuse le travail modifié. recover_owned exige propriétaire et candidat persistés exacts.
 * checkout_status_json_ex lit journal/reçu exact par le cœur sans application. Un reçu terminé prouve
 * l’application passée par propriétaire/source/cible ; une édition ultérieure reste du travail séparé.
 * Une session est liée à un chemin de Store, contexte enregistré et dossier de travail. Les mutations
 * invalident la continuité d’observation. En erreur, recharger : la publication peut être terminée.
 * Voir les [actions natives](doc:workspace-abi#native-workspace-actions) pour reprise et verrouillage.
 *
 * ## Observation et annulation {#operations}
 * Une source d'annulation est à usage unique ; la demande est autorisée depuis un autre thread ou
 * un callback de progression. Sérialiser sa destruction avec ses utilisateurs. Les chaînes du
 * callback sont empruntées uniquement pendant l'appel. Voir les [sessions](doc:workspace-abi),
 * le [binding Go](doc:go-binding) et les [plages vérifiées](doc:file-ranges) pour les contrats et exemples.
 */
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
/* Handle-local native workspace selection. NULL/empty selects primary. Named
 * contexts must already be registered; failure preserves the old selection. */
SYRNCK_API int syrnck_store_use_workspace(SyrnckStore *store, const char *name);
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

/* Create an unpinned object and return out_id[65]. Chunk bytes remain exact;
 * manifest/revision JSON is schema-validated and canonically encoded by C++.
 * Duplicate fields, excessive nesting and inputs/results above 16 MiB fail.
 * This does not validate the object's dependency closure or publish a ref.
 * out_id starts empty on failure; normal required-reader feature gates apply. */
SYRNCK_API int syrnck_object_put(SyrnckStore *store, const char *kind, const unsigned char *data,
                                  size_t length, char *out_id);
/* Bounded (<=16 MiB) in-memory ingestion, without plaintext temporary files.
 * NULL/0 accepts an empty file. Chunking matches ingest_file_ex; NULL is default.
 * Returns an UNPINNED manifest, unlike legacy ingest_file_ex. Publish its typed
 * root (or a containing revision) before releasing the Store to GC. Operation
 * cancellation/progress follow workspace *_ex; complete unreferenced objects
 * can remain after cancellation. No refs or plaintext temporary files are written. */
SYRNCK_API int syrnck_ingest_buffer_ex(SyrnckStore *store, const unsigned char *data, size_t length,
                                        const SyrnckChunking *chunking, const SyrnckOperation *operation,
                                        char *out_id);
/* The same unpinned preparation for a streamed file, without the 16 MiB input
 * buffer bound. Concurrent source writers are the caller's responsibility. */
SYRNCK_API int syrnck_prepare_file_ex(SyrnckStore *store, const char *path, const SyrnckChunking *chunking,
                                       const SyrnckOperation *operation, char *out_id);
/* Typed reference table JSON {name:{kind,id}}; length includes NUL. The standard
 * (NULL,0) query / caller-owned capacity convention applies. No refs are changed. */
SYRNCK_API int syrnck_refs_json(SyrnckStore *store, char *out, size_t capacity, size_t *length);
/* Atomically compare and update one group. Input is an array of exactly
 * {name,expected,value}; expected/value are {kind,id} or null. Null expected
 * requires absence; null value removes. At most 65536 entries / 16 MiB input.
 * Duplicate names/fields fail. Every replacement closure and whole-file hash is
 * verified before one refs publication. Stale expectations return 0, applied=0;
 * complete success returns 0, applied=1. Errors/cancellation reset applied=0;
 * that output proves nothing on nonzero status. An IO error can follow atomic
 * publication: reload the table and prove the outcome before retrying.
 * Successful publication wins a late cancellation. Callers must refresh/replan
 * after applied=0; never retarget an earlier plan to a newly observed value. */
SYRNCK_API int syrnck_refs_compare_json(SyrnckStore *store, const unsigned char *data, size_t length,
                                        const SyrnckOperation *operation, int *applied);

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
/* Additive ABI 1: observe a registered linked context with an external Store.
 * Set the same context on the Store before status; registration/path are
 * verified using that Store's key. NULL/empty name retains primary behavior. */
SYRNCK_API int syrnck_workspace_open_registered_ex(const char *directory, const char *store_path,
                                        const char *name, const SyrnckWorkspaceOptions *options,
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
 * selected HEAD target (named branch or detached revision) for modes 0/2;
 * mode 1 requires a revision selector.
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
/* Native workspace mutation, ABI 1 additive symbols. Every call verifies the
 * exact Store path, selected registered context and workspace directory. An
 * expectation is mandatory for capture/plan/checkout: target is the selected
 * native target (including detached), revision=NULL requires an unborn head.
 * materialization is its exact policy object ID; NULL requires no policy ref.
 * Read state first, then use its exact target/revision; stale values fail before
 * capture, journals or working-file changes. Never retarget a prior operation. */
typedef struct SyrnckNativeExpected {
  const char *target;
  const char *revision;
  const char *materialization;
} SyrnckNativeExpected;
/* Allocated JSON uses string_free; outputs reset to NULL/0 on nonzero status.
 * State is read-only and includes target/reference, typed revision or null,
 * HEAD, materialization, staging flag and pending checkout status. It remains
 * readable during an interrupted checkout; it does not recover anything. */
SYRNCK_API int syrnck_workspace_native_state_json_ex(SyrnckWorkspace *, SyrnckStore *,
    const SyrnckOperation *, char **out, size_t *length);
/* Read one exact native journal/receipt through the core decoder. transaction
 * must be its 64-hex ID. No recovery or refs mutation. A completed receipt
 * proves past application, not that working files are still clean. */
SYRNCK_API int syrnck_workspace_checkout_status_json_ex(SyrnckWorkspace *, SyrnckStore *,
    const char *transaction, const SyrnckOperation *, char **out, size_t *length);
/* Capture selected working files into a retained native revision. Existing
 * native ignore/partial/detached/authorship rules apply. Message <=64 KiB.
 * Output {version,revision}; existing pending/staging work is never taken over. */
SYRNCK_API int syrnck_workspace_capture_json_ex(SyrnckWorkspace *, SyrnckStore *,
    const SyrnckNativeExpected *, const char *message, const SyrnckOperation *, char **out, size_t *length);
/* Plan verifies the candidate closure without changing working files or refs.
 * A blocked plan is a successful report with ready=false. Checkout revalidates
 * all guards and uses the native recoverable journal with the exact 64-hex owner.
 * It requires a clean selected workspace; it never hard-resets dirty work. */
SYRNCK_API int syrnck_workspace_checkout_plan_json_ex(SyrnckWorkspace *, SyrnckStore *,
    const SyrnckNativeExpected *, const char *revision, const SyrnckOperation *, char **out, size_t *length);
SYRNCK_API int syrnck_workspace_checkout_owned_json_ex(SyrnckWorkspace *, SyrnckStore *,
    const SyrnckNativeExpected *, const char *revision, const char *owner, const SyrnckOperation *,
    char **out, size_t *length);
/* Recover only a currently pending journal whose owner, candidate, selected
 * target, workspace and context match exactly. No pending journal is an error.
 * A foreign/manual journal stays explicit. Candidate/owner must be established
 * from durable service state, never inferred from untrusted network fields. */
SYRNCK_API int syrnck_workspace_recover_owned_json_ex(SyrnckWorkspace *, SyrnckStore *,
    const char *target, const char *revision, const char *owner, const SyrnckOperation *,
    char **out, size_t *length);
/* Serialize these calls with wait/status/Close and all Store calls. Mutations
 * invalidate observation continuity; the next Status performs a complete scan.
 * Successful publication wins late cancellation. Any nonzero mutation result
 * has an unknown outcome: re-read native state/journal before planning recovery
 * or retry. Complete objects or a recoverable journal can survive cancellation.
 * These calls never hold a Store after returning; close it before network waits. */
#ifdef __cplusplus
}
#endif
#endif
