package main

import (
	"errors"
	"fmt"
	"io"
	"os"

	syrnck "syrnck.local/bindings/go"
)

func validEnvironmentName(name string) bool {
	if len(name) == 0 || len(name) > 128 {
		return false
	}
	for i, c := range name {
		if !(c == '_' || c >= 'A' && c <= 'Z' || i > 0 && c >= '0' && c <= '9') {
			return false
		}
	}
	return true
}

// The native owner is unlocked once before any Folder workers start, and closed
// after they have stopped. No plaintext storage key crosses the Go ABI.
func loadStorageKey(config configuration, directory, storage string) (*syrnck.SecretKey, error) {
	if config.KeyFile == "" {
		return nil, nil
	}
	if os.Getenv(config.KeyEnv) != "" {
		return nil, errors.New("key_file conflicts with a nonempty raw storage key environment variable")
	}
	passwordValue := os.Getenv(config.KeyPasswordEnv)
	if len(passwordValue) == 0 || len(passwordValue) > 4096 {
		return nil, errors.New("key password environment variable must contain 1..4096 bytes")
	}
	password := []byte(passwordValue)
	defer clear(password)
	envelope, err := protectedEnvelope(config.KeyFile, "key_file", "storage", directory, storage)
	if err != nil {
		return nil, err
	}
	return syrnck.OpenProtectedKey(envelope, password)
}
func protectedEnvelope(path, label, purpose, directory, storage string) ([]byte, error) {
	// Compare the opened object as well as the pathname. A linked input must not
	// silently select a different envelope, directory, device, or unbounded file.
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() != syrnck.ProtectedKeySize {
		return nil, fmt.Errorf("%s must be one regular non-linked 104-byte envelope", label)
	}
	resolved, err := canonicalExisting(path)
	if err != nil {
		return nil, err
	}
	selected, err := keyInWorkingTree(directory, storage, resolved)
	if err != nil {
		return nil, err
	}
	if selected {
		return nil, fmt.Errorf("protected %s key must be outside the workspace or inside its protected Store", purpose)
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil {
		return nil, err
	}
	if !opened.Mode().IsRegular() || opened.Size() != syrnck.ProtectedKeySize || !os.SameFile(info, opened) {
		return nil, fmt.Errorf("%s changed while opening", label)
	}
	envelope, err := io.ReadAll(io.LimitReader(file, syrnck.ProtectedKeySize+1))
	if err != nil {
		return nil, err
	}
	return envelope, nil
}
