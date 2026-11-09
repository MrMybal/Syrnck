package syrnck

import (
	"context"
	"encoding/json"
	"errors"
)

// NativeIdentity is a local revision author/committer, separate from a TLS peer
// or an authorized server account. TimezoneOffset is minutes east of UTC.
type NativeIdentity struct {
	Name           string `json:"name"`
	Email          string `json:"email"`
	TimezoneOffset int32  `json:"timezone_offset"`
}

// UseNativeIdentity configures this Store handle only. An absent seed selects
// unsigned authorship; 32 bytes select native Ed25519 signing. The engine copies
// and owns its signing key until replacement, clearing, or Close. Callers own
// their seed slice and must serialize its use and wipe it when no longer needed.
func (s *Store) UseNativeIdentity(identity NativeIdentity, seed []byte) error {
	return s.ConfigureNativeIdentity(identity, NativeIdentityOptions{Seed: seed})
}

type NativeIdentityOptions struct {
	Seed []byte
	// A missing seed still permits reading/recovering retained signed targets;
	// fresh commits fail rather than silently downgrading to unsigned revisions.
	RequireSignature bool
}

func (s *Store) ConfigureNativeIdentity(identity NativeIdentity, options NativeIdentityOptions) error {
	seed := options.Seed
	name, err := text(identity.Name)
	if err != nil {
		return err
	}
	email, err := text(identity.Email)
	if err != nil {
		return err
	}
	if len(seed) != 0 && len(seed) != 32 {
		return errors.New("syrnck: signing seed must contain exactly 32 bytes or be absent")
	}
	return s.with(func() error {
		return nativeUseIdentity(s.handle, name, email, identity.TimezoneOffset, seed, options.RequireSignature)
	})
}

func (s *Store) ClearNativeIdentity() error {
	return s.with(func() error { return nativeClearIdentity(s.handle) })
}

// NativeIdentityMetadata returns public identity/key identifiers or JSON null.
// Secrets are never exported or saved in Store metadata or reference tables.
func (s *Store) NativeIdentityMetadata() (result json.RawMessage, err error) {
	err = s.with(func() error { result, err = nativeIdentityMetadata(s.handle); return err })
	return
}

func (s *Store) PrepareNativeRevision(draft json.RawMessage) (ObjectKey, error) {
	return s.PrepareNativeRevisionContext(context.Background(), draft)
}

// PrepareNativeRevisionContext verifies a fresh v1 draft's complete dependency
// closure and applies this handle's optional author/signature. The result is
// unpinned. Retain it using guarded refs before releasing Store to GC. No working
// files or refs change. A v2 input is refused; this is not a history rewrite API.
func (s *Store) PrepareNativeRevisionContext(ctx context.Context, draft json.RawMessage) (result ObjectKey, err error) {
	if len(draft) > MaxObjectSize {
		return result, errors.New("syrnck: object too large")
	}
	err = s.withContext(ctx, func() error {
		return withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
			id, e := nativePrepareRevision(s.handle, draft, cancellation)
			if e == nil {
				result = ObjectKey{Kind: "revision", ID: id}
			}
			return e
		})
	})
	return
}
