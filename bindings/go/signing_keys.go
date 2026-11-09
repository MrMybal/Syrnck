package syrnck

import (
	"encoding/json"
	"errors"
	"runtime"
)

var ErrSigningKeyClosed = errors.New("syrnck: signing key is closed")
var zeroSigningKeyHandle nativeSigningKeyHandle

// ProtectSigningKey seals a 32-byte Ed25519 seed, or generates a new signer for
// an empty seed. Purpose-specific envelopes cannot be opened as storage keys.
// Inputs are borrowed synchronously; callers own and wipe their passwords/seeds.
func ProtectSigningKey(seed, password []byte, options PasswordKdfOptions) ([]byte, error) {
	if len(seed) != 0 && len(seed) != 32 {
		return nil, errors.New("syrnck: signing seed must contain exactly 32 bytes")
	}
	if err := keyPassword(password); err != nil {
		return nil, err
	}
	costs, err := passwordCost(options)
	if err != nil {
		return nil, err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	return nativeProtectSigningKey(seed, password, costs)
}

// ChangeSigningKeyPassword keeps the signer with fresh salt/nonce. Zero costs
// preserve existing costs. Older envelopes remain valid; this is not revocation.
func ChangeSigningKeyPassword(envelope, before, after []byte, options PasswordKdfOptions) ([]byte, error) {
	if len(envelope) != ProtectedKeySize {
		return nil, errors.New("syrnck: invalid protected signing key envelope")
	}
	if err := keyPassword(before); err != nil {
		return nil, err
	}
	if err := keyPassword(after); err != nil {
		return nil, err
	}
	costs, err := passwordCost(options)
	if err != nil {
		return nil, err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	return nativeChangeSigningKeyPassword(envelope, before, after, costs)
}

// SigningKey owns an authenticated Ed25519 seed in native memory without a raw
// export. Do not copy it. Close explicitly; calls/Close are serialized. Reuse
// with NativeIdentityOptions.Signer for short Store sessions without another KDF.
// Each configured Store retains its own expanded key, independent of this owner.
type SigningKey struct {
	mu     contextMutex
	handle nativeSigningKeyHandle
}

func OpenProtectedSigner(envelope, password []byte) (*SigningKey, error) {
	if len(envelope) != ProtectedKeySize {
		return nil, errors.New("syrnck: invalid protected signing key envelope")
	}
	if err := keyPassword(password); err != nil {
		return nil, err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	handle, err := nativeOpenSigner(envelope, password)
	if err != nil {
		return nil, err
	}
	return &SigningKey{handle: handle}, nil
}
func (key *SigningKey) with(fn func(nativeSigningKeyHandle) error) error {
	if key == nil {
		return ErrSigningKeyClosed
	}
	key.mu.Lock()
	defer key.mu.Unlock()
	if key.handle == zeroSigningKeyHandle {
		return ErrSigningKeyClosed
	}
	return fn(key.handle)
}
func (key *SigningKey) Close() error {
	if key == nil {
		return nil
	}
	key.mu.Lock()
	defer key.mu.Unlock()
	if key.handle == zeroSigningKeyHandle {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	err := nativeCloseSigner(key.handle)
	key.handle = zeroSigningKeyHandle
	return err
}

// PublicMetadata returns algorithm, public_key and key_id JSON, never the seed.
func (key *SigningKey) PublicMetadata() (result json.RawMessage, err error) {
	err = key.with(func(handle nativeSigningKeyHandle) error {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		result, err = nativeSignerMetadata(handle)
		return err
	})
	return
}
