package syrnck

import (
	"errors"
	"runtime"
)

const ProtectedKeySize = 104

var ErrSecretKeyClosed = errors.New("syrnck: secret key is closed")
var zeroSecretKeyHandle nativeSecretKeyHandle

// PasswordKdfOptions selects Argon2id costs. Nonzero MemoryKiB must be 64..256
// whole MiB; nonzero Passes must be 3..10. Zero fields select defaults (64 MiB,
// three passes) when protecting, or preserve old costs when changing a password.
type PasswordKdfOptions struct{ MemoryKiB, Passes uint32 }

func passwordCost(options PasswordKdfOptions) (*[2]uint32, error) {
	if options.MemoryKiB != 0 && (options.MemoryKiB < 65536 || options.MemoryKiB > 262144 || options.MemoryKiB%1024 != 0) {
		return nil, errors.New("syrnck: password KDF memory must be 64..256 whole MiB")
	}
	if options.Passes != 0 && (options.Passes < 3 || options.Passes > 10) {
		return nil, errors.New("syrnck: password KDF passes must be in 3..10")
	}
	if options.MemoryKiB == 0 && options.Passes == 0 {
		return nil, nil
	}
	return &[2]uint32{options.MemoryKiB, options.Passes}, nil
}
func keyPassword(password []byte) error {
	if len(password) < 1 || len(password) > 4096 {
		return errors.New("syrnck: key password must contain 1..4096 bytes")
	}
	return nil
}

// ProtectKey seals exactly 32 caller-owned key bytes, or generates a random key
// when key is empty. It returns only an encrypted envelope. Inputs are borrowed
// for the synchronous call; the caller owns and clears password/key buffers.
func ProtectKey(key, password []byte, options PasswordKdfOptions) ([]byte, error) {
	if len(key) == 0 {
		key = nil
	}
	if len(key) != 0 && len(key) != 32 {
		return nil, errors.New("syrnck: key must contain exactly 32 bytes")
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
	return nativeProtectKey(key, password, costs)
}

// ChangeKeyPassword rewraps the same secret with fresh salt/nonce. Zero cost
// fields preserve old costs. Old envelopes remain usable; this is not revocation
// or Store-object rotation. No input is changed and no raw key is returned.
func ChangeKeyPassword(envelope, oldPassword, newPassword []byte, options PasswordKdfOptions) ([]byte, error) {
	if len(envelope) != ProtectedKeySize {
		return nil, errors.New("syrnck: invalid protected key envelope")
	}
	if err := keyPassword(oldPassword); err != nil {
		return nil, err
	}
	if err := keyPassword(newPassword); err != nil {
		return nil, err
	}
	costs, err := passwordCost(options)
	if err != nil {
		return nil, err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	return nativeChangeKeyPassword(envelope, oldPassword, newPassword, costs)
}

// SecretKey owns a native unwrapped key and must not be copied. Close explicitly
// to wipe it. It exposes no plaintext export. Calls and Close are serialized;
// caller-owned input buffers are not retained or cleared by this object.
type SecretKey struct {
	mu     contextMutex
	handle nativeSecretKeyHandle
}

// OpenProtectedKey authenticates once; reuse the resulting owner for short Store
// sessions instead of repeating the memory-hard KDF. Derivation is synchronous.
func OpenProtectedKey(envelope, password []byte) (*SecretKey, error) {
	if len(envelope) != ProtectedKeySize {
		return nil, errors.New("syrnck: invalid protected key envelope")
	}
	if err := keyPassword(password); err != nil {
		return nil, err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	handle, err := nativeOpenKey(envelope, password)
	if err != nil {
		return nil, err
	}
	return &SecretKey{handle: handle}, nil
}
func (key *SecretKey) Close() error {
	key.mu.Lock()
	defer key.mu.Unlock()
	if key.handle == zeroSecretKeyHandle {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	err := nativeCloseKey(key.handle)
	key.handle = zeroSecretKeyHandle
	return err
}

// Open borrows the native key only while constructing a Store. The Store keeps
// its own key, so closing SecretKey does not revoke or close existing Stores.
func (key *SecretKey) Open(path string, create bool) (*Store, error) {
	value, err := text(path)
	if err != nil {
		return nil, err
	}
	key.mu.Lock()
	defer key.mu.Unlock()
	if key.handle == zeroSecretKeyHandle {
		return nil, ErrSecretKeyClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	handle, err := nativeOpenWithSecret(value, create, key.handle)
	if err != nil {
		return nil, err
	}
	return &Store{handle: handle, order: sequence.Add(1)}, nil
}

// OpenProtected is a one-shot convenience. For resident services use
// OpenProtectedKey and reuse its owner. Authentication failure does not create
// a Store directory; this function never exposes the unwrapped key to Go.
func OpenProtected(path string, envelope, password []byte, create bool) (*Store, error) {
	if _, err := text(path); err != nil {
		return nil, err
	}
	key, err := OpenProtectedKey(envelope, password)
	if err != nil {
		return nil, err
	}
	defer key.Close()
	return key.Open(path, create)
}
