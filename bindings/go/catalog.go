package syrnck

import (
	"context"
	"encoding/json"
	"errors"
)

// RefUpdate is one expected-value condition and replacement in an atomic group.
// Nil Expected requires absence; nil Value removes the reference. A failed
// comparison publishes none of the group. Refresh/replan instead of retargeting
// an earlier operation to a newly observed value.
type RefUpdate struct {
	Name     string     `json:"name"`
	Expected *ObjectKey `json:"expected"`
	Value    *ObjectKey `json:"value"`
}

func chunkingConfig(value Chunking) *[4]uint32 {
	if value == (Chunking{}) {
		return nil
	}
	var algorithm uint32
	if value.Fixed {
		algorithm = 1
	}
	return &[4]uint32{algorithm, value.Min, value.Average, value.Max}
}

// Put creates an unpinned typed object. C++ computes its identity and validates
// and canonically encodes manifest/revision JSON, rejecting duplicate fields.
// Chunk data stays exact. It does not verify dependencies or publish a root.
func (s *Store) Put(kind string, data []byte) (ObjectKey, error) {
	return s.PutContext(context.Background(), kind, data)
}

// PutContext checks cancellation while waiting for serialization and before the
// bounded (<=16 MiB) native call. Encoding/compression already in flight finishes.
func (s *Store) PutContext(ctx context.Context, kind string, data []byte) (result ObjectKey, err error) {
	if kind != "chunk" && kind != "manifest" && kind != "revision" {
		return result, errors.New("syrnck: invalid object kind")
	}
	if len(data) > MaxObjectSize {
		return result, errors.New("syrnck: object too large")
	}
	name, _ := text(kind)
	err = s.withContext(ctx, func() error {
		id, e := nativePut(s.handle, name, data)
		if e == nil {
			result = ObjectKey{Kind: kind, ID: id}
		}
		return e
	})
	return
}

// PrepareBytes ingests bounded memory without a plaintext temporary file or
// implicit ffi/<id> root. Retain the returned manifest or a containing revision
// before releasing the Store to GC. Unlike legacy Ingest, this never pins it.
func (s *Store) PrepareBytes(data []byte, chunking Chunking) (string, error) {
	return s.PrepareBytesContext(context.Background(), data, chunking)
}
func (s *Store) PrepareBytesContext(ctx context.Context, data []byte, chunking Chunking) (id string, err error) {
	if len(data) > MaxObjectSize {
		return "", errors.New("syrnck: object too large")
	}
	err = s.withContext(ctx, func() error {
		return withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
			id, err = nativePrepareBuffer(s.handle, data, chunkingConfig(chunking), cancellation)
			return err
		})
	})
	return
}

// PrepareFile streams an unpinned file without the memory buffer limit. Keep
// external writers stable when a coherent source snapshot is required.
func (s *Store) PrepareFile(path string, chunking Chunking) (string, error) {
	return s.PrepareFileContext(context.Background(), path, chunking)
}
func (s *Store) PrepareFileContext(ctx context.Context, path string, chunking Chunking) (id string, err error) {
	p, err := text(path)
	if err != nil {
		return "", err
	}
	err = s.withContext(ctx, func() error {
		return withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
			id, err = nativePrepareFile(s.handle, p, chunkingConfig(chunking), cancellation)
			return err
		})
	})
	return
}

func (s *Store) Refs() (map[string]ObjectKey, error) { return s.RefsContext(context.Background()) }

// RefsContext returns an independent typed table. Cancellation is checked before
// the bounded read. A subsequent CompareRefs must still use expected values.
func (s *Store) RefsContext(ctx context.Context) (result map[string]ObjectKey, err error) {
	err = s.withContext(ctx, func() error {
		data, e := nativeRefs(s.handle)
		if e != nil {
			return e
		}
		return json.Unmarshal(data, &result)
	})
	return
}

func (s *Store) CompareRefs(updates []RefUpdate) (bool, error) {
	return s.CompareRefsContext(context.Background(), updates)
}

// CompareRefsContext verifies replacement closures/file hashes and atomically
// publishes the complete group. False with nil error means stale expectations.
// Cancellation is propagated through verification and before publication; a
// completed publication wins a later cancellation. Empty/nil groups are no-ops.
// On error the boolean is not publication proof: IO can fail after an atomic
// replacement. Reload and establish the outcome before any retry.
func (s *Store) CompareRefsContext(ctx context.Context, updates []RefUpdate) (applied bool, err error) {
	if len(updates) > 65536 {
		return false, errors.New("syrnck: reference transaction exceeds 65536 entries")
	}
	for _, update := range updates {
		if len(update.Name) > 200 {
			return false, errors.New("syrnck: invalid reference name")
		}
		if _, e := text(update.Name); e != nil {
			return false, e
		}
		for _, key := range []*ObjectKey{update.Expected, update.Value} {
			if key == nil {
				continue
			}
			if len(key.Kind) > 8 || len(key.ID) != 64 {
				return false, errors.New("syrnck: invalid reference object key")
			}
			if _, _, e := keyText(*key); e != nil {
				return false, e
			}
		}
	}
	if updates == nil {
		updates = []RefUpdate{}
	}
	data, err := json.Marshal(updates)
	if err != nil {
		return false, err
	}
	if len(data) > MaxObjectSize {
		return false, errors.New("syrnck: reference transaction too large")
	}
	err = s.withContext(ctx, func() error {
		return withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
			applied, err = nativeCompareRefs(s.handle, data, cancellation)
			return err
		})
	})
	return
}
