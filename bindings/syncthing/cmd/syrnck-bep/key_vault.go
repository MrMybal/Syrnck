package main

import (
	"errors"
	"path/filepath"
	"runtime"
	"strings"
	"sync"

	syrnck "syrnck.local/bindings/go"
)

func selectedVaultReference(encoded string, purpose syrnck.VaultKeyPurpose) (syrnck.VaultReference, error) {
	ref, err := syrnck.ParseVaultReference(encoded)
	if err != nil {
		return "", err
	}
	actual, err := ref.Purpose()
	if err != nil {
		return "", err
	}
	if actual != purpose {
		return "", errors.New("vault reference has another key purpose")
	}
	return ref, nil
}

// Validate before workers, filesystem capture or network activity. References
// are public account-local selectors, never portable encryption keys.
func validateVaultConfiguration(config configuration) error {
	if config.KeyVault != "" {
		if config.KeyFile != "" {
			return errors.New("choose key_file or key_vault")
		}
		if config.KeyPasswordEnv != "" {
			return errors.New("key_password_env requires key_file")
		}
		if _, err := selectedVaultReference(config.KeyVault, syrnck.VaultStorageKey); err != nil {
			return err
		}
	}
	if config.SigningKeyVault != "" {
		if config.SigningKeyFile != "" || config.SigningKeyEnv != "" {
			return errors.New("choose signing_key_env, signing_key_file or signing_key_vault")
		}
		if config.SigningKeyPasswordEnv != "" {
			return errors.New("signing_key_password_env requires signing_key_file")
		}
		if config.Author == nil {
			return errors.New("signing_key_vault requires an explicit author")
		}
		if _, err := selectedVaultReference(config.SigningKeyVault, syrnck.VaultSigningKey); err != nil {
			return err
		}
	}
	if config.VaultKeychain != "" {
		if config.KeyVault == "" && config.SigningKeyVault == "" {
			return errors.New("vault_keychain requires key_vault or signing_key_vault")
		}
		if runtime.GOOS != "darwin" || !filepath.IsAbs(config.VaultKeychain) ||
			len(config.VaultKeychain) > 32768 || strings.ContainsRune(config.VaultKeychain, 0) {
			return errors.New("vault_keychain must name an absolute macOS keychain")
		}
	}
	return nil
}

var vaultProcessPolicy struct {
	once sync.Once
	err  error
}

func openSelectedVault(config configuration) (*syrnck.SystemKeyVault, error) {
	vault, err := syrnck.OpenSystemKeyVault(config.VaultKeychain)
	if err != nil {
		return nil, err
	}
	// This standalone executable owns a headless process. Opt into macOS's
	// process-wide Keychain policy only when a vault has actually been selected.
	// Native library users choose their own policy explicitly.
	vaultProcessPolicy.once.Do(func() { vaultProcessPolicy.err = syrnck.DisableMacOSKeychainInteraction() })
	if vaultProcessPolicy.err != nil {
		vault.Close()
		return nil, vaultProcessPolicy.err
	}
	return vault, nil
}
