package syrnck

import (
	"encoding/json"
	"errors"
	"runtime"
	"strings"
)

type VaultKeyPurpose uint32

const (
	VaultStorageKey VaultKeyPurpose = 1
	VaultSigningKey VaultKeyPurpose = 2
	VaultSealedKey  VaultKeyPurpose = 3
)

// VaultReference is a public local OS-account reference, not a credential or a
// portable backup. Every operation validates its version, purpose and ID.
type VaultReference string

func (ref VaultReference) String() string { return string(ref) }
func ParseVaultReference(value string) (VaultReference, error) {
	_, err := VaultReference(value).Purpose()
	if err != nil {
		return "", err
	}
	return VaultReference(value), nil
}
func (ref VaultReference) Purpose() (VaultKeyPurpose, error) {
	if len(ref) > 96 {
		return 0, errors.New("syrnck: invalid system key vault reference")
	}
	parts := strings.Split(string(ref), ":")
	if len(parts) != 4 || parts[0] != "syrnck-vault" || parts[1] != "1" || len(parts[3]) != 64 {
		return 0, errors.New("syrnck: invalid system key vault reference")
	}
	for _, c := range []byte(parts[3]) {
		if !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') {
			return 0, errors.New("syrnck: invalid system key vault reference")
		}
	}
	switch parts[2] {
	case "storage":
		return VaultStorageKey, nil
	case "signing":
		return VaultSigningKey, nil
	case "sealed":
		return VaultSealedKey, nil
	}
	return 0, errors.New("syrnck: invalid system key vault reference")
}
func vaultPurpose(purpose VaultKeyPurpose) error {
	if purpose < VaultStorageKey || purpose > VaultSealedKey {
		return errors.New("syrnck: invalid system key vault purpose")
	}
	return nil
}
func vaultReference(ref VaultReference, purpose VaultKeyPurpose) ([]byte, error) {
	actual, err := ref.Purpose()
	if err != nil {
		return nil, err
	}
	if purpose != 0 && actual != purpose {
		return nil, errors.New("syrnck: system key vault reference has another purpose")
	}
	return text(string(ref))
}

var ErrSystemKeyVaultClosed = errors.New("syrnck: system key vault is closed")
var zeroSystemVaultHandle nativeSystemVaultHandle

// DisableMacOSKeychainInteraction explicitly makes ALL Keychain operations in
// this process noninteractive on macOS. Call before workers and do not re-enable
// interaction while vaults are in use. Vault calls otherwise refuse file-based
// keychains rather than changing the process policy implicitly. No-op elsewhere.
func DisableMacOSKeychainInteraction() error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	return nativeDisableMacOSKeychainInteraction()
}

// SystemKeyVault owns native provider configuration, not an unlocked session or
// any secret. Do not copy it. Calls and Close are serialized. Use an empty path
// for platform defaults; an absolute selected keychain is supported only on
// macOS. Real operations can fail because the OS account vault is unavailable.
type SystemKeyVault struct {
	mu     contextMutex
	handle nativeSystemVaultHandle
}

func OpenSystemKeyVault(macosKeychain string) (*SystemKeyVault, error) {
	path, err := text(macosKeychain)
	if err != nil {
		return nil, err
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	handle, err := nativeOpenSystemVault(path)
	if err != nil {
		return nil, err
	}
	return &SystemKeyVault{handle: handle}, nil
}
func (vault *SystemKeyVault) with(fn func(nativeSystemVaultHandle) error) error {
	if vault == nil {
		return ErrSystemKeyVaultClosed
	}
	vault.mu.Lock()
	defer vault.mu.Unlock()
	if vault.handle == zeroSystemVaultHandle {
		return ErrSystemKeyVaultClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	return fn(vault.handle)
}
func (vault *SystemKeyVault) Close() error {
	if vault == nil {
		return nil
	}
	vault.mu.Lock()
	defer vault.mu.Unlock()
	if vault.handle == zeroSystemVaultHandle {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	err := nativeCloseSystemVault(vault.handle)
	vault.handle = zeroSystemVaultHandle
	return err
}

// VaultProvider describes compiled support. Available does not prove that an
// account logon session, selected keychain or Secret Service is unlocked.
type VaultProvider struct {
	Version   int    `json:"version"`
	Provider  string `json:"provider"`
	Available bool   `json:"available"`
}

func (vault *SystemKeyVault) Provider() (result VaultProvider, err error) {
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		data, err := nativeSystemVaultProvider(handle)
		if err != nil {
			return err
		}
		if err = json.Unmarshal(data, &result); err != nil {
			return err
		}
		if result.Version != 1 || result.Provider == "" || result.Available != (result.Provider != "unavailable") {
			return errors.New("syrnck: invalid native system vault metadata")
		}
		return nil
	})
	return
}

// Generate creates a random native secret and a fresh independent vault entry.
// There is no caller-selected target name, entry update or raw secret export.
func (vault *SystemKeyVault) Generate(purpose VaultKeyPurpose) (ref VaultReference, err error) {
	if err = vaultPurpose(purpose); err != nil {
		return
	}
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		var callErr error
		ref, callErr = nativeSystemVaultGenerate(handle, purpose)
		return callErr
	})
	return
}

// ImportProtected authenticates a 104-byte purpose-specific envelope before
// creating a fresh entry. Password/envelope buffers stay caller-owned.
func (vault *SystemKeyVault) ImportProtected(purpose VaultKeyPurpose, envelope, password []byte) (ref VaultReference, err error) {
	if err = vaultPurpose(purpose); err != nil {
		return
	}
	if len(envelope) != ProtectedKeySize {
		return "", errors.New("syrnck: invalid protected key envelope")
	}
	if err = keyPassword(password); err != nil {
		return
	}
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		var callErr error
		ref, callErr = nativeSystemVaultImport(handle, purpose, envelope, password)
		return callErr
	})
	return
}

// ExportProtected returns only a purpose-separated password envelope. Save and
// verify an independent backup before relying on the machine/account vault.
func (vault *SystemKeyVault) ExportProtected(ref VaultReference, password []byte, options PasswordKdfOptions) (result []byte, err error) {
	reference, err := vaultReference(ref, 0)
	if err != nil {
		return nil, err
	}
	if err = keyPassword(password); err != nil {
		return nil, err
	}
	costs, err := passwordCost(options)
	if err != nil {
		return nil, err
	}
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		var callErr error
		result, callErr = nativeSystemVaultExport(handle, reference, password, costs)
		return callErr
	})
	return
}

// OpenSecret accepts storage references only. The returned existing key owner
// can open short Store sessions without exporting key bytes to Go.
func (vault *SystemKeyVault) OpenSecret(ref VaultReference) (result *SecretKey, err error) {
	reference, err := vaultReference(ref, VaultStorageKey)
	if err != nil {
		return nil, err
	}
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		key, err := nativeSystemVaultOpenSecret(handle, reference)
		if err == nil {
			result = &SecretKey{handle: key}
		}
		return err
	})
	return
}
func (vault *SystemKeyVault) OpenSigner(ref VaultReference) (result *SigningKey, err error) {
	reference, err := vaultReference(ref, VaultSigningKey)
	if err != nil {
		return nil, err
	}
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		key, err := nativeSystemVaultOpenSigner(handle, reference)
		if err == nil {
			result = &SigningKey{handle: key}
		}
		return err
	})
	return
}
func (vault *SystemKeyVault) OpenSealed(ref VaultReference) (result *SealedKey, err error) {
	reference, err := vaultReference(ref, VaultSealedKey)
	if err != nil {
		return nil, err
	}
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		key, err := nativeSystemVaultOpenSealed(handle, reference)
		if err == nil {
			result = &SealedKey{handle: key}
		}
		return err
	})
	return
}

// Erase removes only the selected entry. Opened native owners, exported
// backups and encrypted repository objects are not revoked or rotated.
func (vault *SystemKeyVault) Erase(ref VaultReference) (removed bool, err error) {
	reference, err := vaultReference(ref, 0)
	if err != nil {
		return false, err
	}
	err = vault.with(func(handle nativeSystemVaultHandle) error {
		var callErr error
		removed, callErr = nativeSystemVaultErase(handle, reference)
		return callErr
	})
	return
}
