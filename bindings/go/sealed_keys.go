package syrnck

import (
	"context"
	"encoding/json"
	"errors"
	"runtime"
)

var ErrSealedKeyClosed = errors.New("syrnck: sealed key is closed")
var zeroSealedKeyHandle nativeSealedKeyHandle

// ProtectSealedKey protects exactly 32 master-key bytes, or generates a random
// master when key is empty. The authenticated purpose differs from Store keys
// and signing seeds. Caller-owned key/password bytes are borrowed, never wiped.
func ProtectSealedKey(key, password []byte, options PasswordKdfOptions) ([]byte, error) {
	if len(key) == 0 {
		key = nil
	}
	if len(key) != 0 && len(key) != 32 {
		return nil, errors.New("syrnck: sealed key must contain exactly 32 bytes")
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
	return nativeProtectSealedKey(key, password, costs)
}

// ChangeSealedKeyPassword preserves the master/realm with fresh salt and nonce.
// Zero cost fields preserve previous costs. Old envelopes remain usable; this
// does not rotate objects or revoke former key holders. No raw export exists.
func ChangeSealedKeyPassword(envelope, before, after []byte, options PasswordKdfOptions) ([]byte, error) {
	if len(envelope) != ProtectedKeySize {
		return nil, errors.New("syrnck: invalid protected sealed key envelope")
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
	return nativeChangeSealedKeyPassword(envelope, before, after, costs)
}

// SealedKey owns an authenticated end-to-end master in native memory. Do not
// copy it. Close explicitly to wipe it. Calls and Close are serialized; this
// owner cannot be used as a Store at-rest key or a native signing seed.
type SealedKey struct {
	mu     contextMutex
	handle nativeSealedKeyHandle
}

// OpenProtectedSealedKey authenticates once. Reuse the owner for short Store
// sessions instead of repeating Argon2id. Password bytes are not retained.
func OpenProtectedSealedKey(envelope, password []byte) (*SealedKey, error) {
	if len(envelope) != ProtectedKeySize {
		return nil, errors.New("syrnck: invalid protected sealed key envelope")
	}
	if err := keyPassword(password); err != nil {
		return nil, err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	handle, err := nativeOpenSealedKey(envelope, password)
	if err != nil {
		return nil, err
	}
	return &SealedKey{handle: handle}, nil
}
func (key *SealedKey) withContext(ctx context.Context, fn func(nativeSealedKeyHandle) error) error {
	if key == nil {
		return ErrSealedKeyClosed
	}
	if err := key.mu.LockContext(ctx); err != nil {
		return err
	}
	defer key.mu.Unlock()
	if key.handle == zeroSealedKeyHandle {
		return ErrSealedKeyClosed
	}
	return fn(key.handle)
}
func (key *SealedKey) Close() error {
	if key == nil {
		return nil
	}
	key.mu.Lock()
	defer key.mu.Unlock()
	if key.handle == zeroSealedKeyHandle {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	err := nativeCloseSealedKey(key.handle)
	key.handle = zeroSealedKeyHandle
	return err
}

// PublicMetadata returns {version,type,realm} JSON, never the master key.
func (key *SealedKey) PublicMetadata() (result json.RawMessage, err error) {
	err = key.withContext(context.Background(), func(handle nativeSealedKeyHandle) error {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		result, err = nativeSealedKeyMetadata(handle)
		return err
	})
	return
}
