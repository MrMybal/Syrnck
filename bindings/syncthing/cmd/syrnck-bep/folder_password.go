package main

import (
	"errors"
	"os"
	"strings"
	"unicode/utf8"
)

func loadFolderPassword(config configuration) (string, error) {
	if !config.UntrustedPeer {
		return "", nil
	}
	// Read once, before capture/listen/workers. The upstream codec retains Go
	// key/password caches; clearing the launcher variable is not secure erasure.
	value, present := os.LookupEnv(config.FolderPasswordEnv)
	if err := os.Unsetenv(config.FolderPasswordEnv); err != nil {
		return "", err
	}
	if !present || len(value) == 0 || len(value) > 4096 || !utf8.ValidString(value) || strings.ContainsRune(value, 0) {
		return "", errors.New("folder password environment variable must contain 1..4096 UTF-8 bytes")
	}
	return value, nil
}
