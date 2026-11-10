package main

import (
	"errors"
	"os"
	syrnck "syrnck.local/bindings/go"
)

// Authenticate before workers/listeners. The typed native owner is shared only
// for synchronous identity attachment; each Store gets independent signing state.
func loadSigningKey(config configuration, directory, storage string) (*syrnck.SigningKey, error) {
	if config.SigningKeyVault != "" {
		if err := validateVaultConfiguration(config); err != nil {
			return nil, err
		}
		ref, err := selectedVaultReference(config.SigningKeyVault, syrnck.VaultSigningKey)
		if err != nil {
			return nil, err
		}
		vault, err := openSelectedVault(config)
		if err != nil {
			return nil, err
		}
		defer vault.Close()
		return vault.OpenSigner(ref)
	}
	if config.SigningKeyFile == "" {
		return nil, nil
	}
	if config.Author == nil {
		return nil, errors.New("signing_key_file requires an explicit author")
	}
	if config.SigningKeyEnv != "" {
		return nil, errors.New("choose signing_key_env or signing_key_file")
	}
	value := os.Getenv(config.SigningKeyPasswordEnv)
	if len(value) == 0 || len(value) > 4096 {
		return nil, errors.New("signing key password environment variable must contain 1..4096 bytes")
	}
	password := []byte(value)
	defer clear(password)
	envelope, err := protectedEnvelope(config.SigningKeyFile, "signing_key_file", "signing", directory, storage)
	if err != nil {
		return nil, err
	}
	owner, err := syrnck.OpenProtectedSigner(envelope, password)
	if err != nil {
		return nil, err
	}
	if err := os.Unsetenv(config.SigningKeyPasswordEnv); err != nil {
		owner.Close()
		return nil, err
	}
	return owner, nil
}
