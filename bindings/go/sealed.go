package syrnck

import (
	"context"
	"encoding/json"
	"errors"
	"runtime"
	"sort"
)

// SealedLimits bounds original closure/trie work. Zero fields select the core
// defaults: 4194304 objects, 268435456 work, 16 MiB parsed-index cache, 64 MiB
// new index entries and 100000 predecessor roots. These do not bound total RSS.
type SealedLimits struct {
	MaxObjects, MaxWork, MaxIndexCacheBytes, MaxNewIndexBytes, MaxHistory uint64
}

func (limits SealedLimits) native() *[5]uint64 {
	if limits == (SealedLimits{}) {
		return nil
	}
	return &[5]uint64{limits.MaxObjects, limits.MaxWork, limits.MaxIndexCacheBytes, limits.MaxNewIndexBytes, limits.MaxHistory}
}

// SealedSnapshotReport contains private original root names/IDs. Keep reports
// on the client. Transmit only the retained opaque Snapshot closure from Wire.
// Fields not applicable to an operation are zero/empty. Snapshot and Index are
// wire revision IDs; Roots contains original typed objects.
type SealedSnapshotReport struct {
	Version              uint32               `json:"version"`
	Realm                string               `json:"realm"`
	Channel              string               `json:"channel"`
	Snapshot             string               `json:"snapshot"`
	Index                string               `json:"index"`
	Roots                map[string]ObjectKey `json:"roots"`
	State                string               `json:"state"`
	RetainedRef          string               `json:"retained_ref"`
	Checkpoint           string               `json:"checkpoint"`
	Objects              uint64               `json:"objects"`
	IndexNodesRead       uint64               `json:"index_nodes_read"`
	IndexNodesWritten    uint64               `json:"index_nodes_written"`
	Work                 uint64               `json:"work"`
	SealedObjects        uint64               `json:"sealed_objects"`
	ReusedObjects        uint64               `json:"reused_objects"`
	ImportedObjects      uint64               `json:"imported_objects"`
	PlaintextBytesSealed uint64               `json:"plaintext_bytes_sealed"`
}

func sealedReport(data []byte, channel string) (result SealedSnapshotReport, err error) {
	if err = json.Unmarshal(data, &result); err != nil {
		return SealedSnapshotReport{}, err
	}
	if result.Version != 1 || result.Channel != channel || len(result.Realm) != 32 ||
		len(result.Snapshot) != 64 || len(result.Index) != 64 || len(result.Roots) < 1 || len(result.Roots) > 100000 {
		return SealedSnapshotReport{}, errors.New("syrnck: invalid native sealed snapshot report")
	}
	return
}

// Lock the key first, then distinct Stores in creation order, for every sealed
// operation. Context cancellation also interrupts serialization waits. No Go
// pointer is retained by native code; Close waits until all native calls finish.
func (key *SealedKey) withStores(ctx context.Context, stores []*Store, fn func(nativeSealedKeyHandle, nativeCancellationHandle) error) error {
	return key.withContext(ctx, func(handle nativeSealedKeyHandle) error {
		ordered := make([]*Store, 0, len(stores))
		seen := make(map[*Store]bool)
		for _, store := range stores {
			if store == nil {
				return ErrClosed
			}
			if !seen[store] {
				ordered = append(ordered, store)
				seen[store] = true
			}
		}
		sort.Slice(ordered, func(i, j int) bool { return ordered[i].order < ordered[j].order })
		for _, store := range ordered {
			if err := store.mu.LockContext(ctx); err != nil {
				return err
			}
			defer store.mu.Unlock()
			if store.handle == zeroHandle {
				return ErrClosed
			}
		}
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		return withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error { return fn(handle, cancellation) })
	})
}
func sealedText(value string, maximum int) ([]byte, error) {
	if len(value) > maximum {
		return nil, errors.New("syrnck: sealed text exceeds its limit")
	}
	return text(value)
}

// SealSnapshot verifies explicit source roots and prepares an opaque snapshot in
// a separate wire Store. previous is an exact earlier sealed ID, or empty for a
// new channel. Complete unchanged roots reuse that ID and existing ciphertext.
// Source refs/files are read-only; wire retains sealed/prepared/ID on success.
// The master, both Stores and their serialization locks live through the call.
func (key *SealedKey) SealSnapshot(ctx context.Context, source, wire *Store, channel string,
	roots map[string]ObjectKey, previous string, limits SealedLimits) (result SealedSnapshotReport, err error) {
	if err = ctx.Err(); err != nil {
		return
	}
	name, err := sealedText(channel, 64)
	if err != nil {
		return result, err
	}
	parent, err := sealedText(previous, 64)
	if err != nil {
		return result, err
	}
	if len(roots) < 1 || len(roots) > 100000 {
		return result, errors.New("syrnck: sealed snapshot requires 1..100000 roots")
	}
	for name, root := range roots {
		if _, err = sealedText(name, 200); err != nil {
			return
		}
		if len(root.Kind) > 8 || len(root.ID) != 64 {
			return result, errors.New("syrnck: invalid sealed root key")
		}
		if _, _, err = keyText(root); err != nil {
			return
		}
	}
	data, err := json.Marshal(roots)
	if err != nil {
		return result, err
	}
	if len(data) > MaxObjectSize {
		return result, errors.New("syrnck: sealed roots exceed 16 MiB")
	}
	err = key.withStores(ctx, []*Store{source, wire}, func(handle nativeSealedKeyHandle, cancellation nativeCancellationHandle) error {
		encoded, e := nativeSealSnapshot(source.handle, wire.handle, handle, name, data, parent, limits.native(), cancellation)
		if e != nil {
			return e
		}
		result, e = sealedReport(encoded, channel)
		return e
	})
	return
}

// InspectSnapshot authenticates/decrypts and audits without importing or changing
// references. The returned private roots are never appropriate server metadata.
func (key *SealedKey) InspectSnapshot(ctx context.Context, wire *Store, snapshot, channel string,
	limits SealedLimits) (result SealedSnapshotReport, err error) {
	name, err := sealedText(channel, 64)
	if err != nil {
		return result, err
	}
	id, err := sealedText(snapshot, 64)
	if err != nil {
		return result, err
	}
	err = key.withStores(ctx, []*Store{wire}, func(handle nativeSealedKeyHandle, cancellation nativeCancellationHandle) error {
		encoded, e := nativeInspectSealed(wire.handle, handle, id, name, limits.native(), cancellation)
		if e != nil {
			return e
		}
		result, e = sealedReport(encoded, channel)
		return e
	})
	return
}

// OpenSnapshot imports verified originals, then atomically retains roots and a
// checkpoint below sealed/checkpoints/. Existing checkpoints reject rollback and
// forks. Use a different checkpoint for explicit historical restoration. Native
// branches/indexes/workspace files are not changed. An error can occur after
// publication; reload refs to prove its outcome before retrying. All Store paths
// must be separate and outside known/conventional plaintext project workspaces.
func (key *SealedKey) OpenSnapshot(ctx context.Context, wire, destination *Store, snapshot, channel, checkpoint string,
	limits SealedLimits) (result SealedSnapshotReport, err error) {
	name, err := sealedText(channel, 64)
	if err != nil {
		return result, err
	}
	id, err := sealedText(snapshot, 64)
	if err != nil {
		return result, err
	}
	ref, err := sealedText(checkpoint, 200)
	if err != nil {
		return result, err
	}
	err = key.withStores(ctx, []*Store{wire, destination}, func(handle nativeSealedKeyHandle, cancellation nativeCancellationHandle) error {
		encoded, e := nativeOpenSealed(wire.handle, destination.handle, handle, id, name, ref, limits.native(), cancellation)
		if e != nil {
			return e
		}
		result, e = sealedReport(encoded, channel)
		return e
	})
	return
}
