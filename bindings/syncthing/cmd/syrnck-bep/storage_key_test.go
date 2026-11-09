package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	syrnck "syrnck.local/bindings/go"
)

func TestProtectedStorageKeyConfiguration(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "service.json")
	base := configuration{Version: 1, Directory: "work", Folder: "project", Certificate: "cert.pem", PrivateKey: "device.pem", Mode: "listen", Address: "127.0.0.1:0", KeyFile: "storage.syrkey"}
	write := func(config configuration) {
		t.Helper()
		data, err := json.Marshal(config)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	write(base)
	loaded, err := loadConfiguration(path)
	if err != nil || loaded.KeyFile != filepath.Join(root, base.KeyFile) || loaded.KeyPasswordEnv != "SYRNCK_KEY_PASSWORD" {
		t.Fatal("file provider defaults/relative resolution", loaded, err)
	}
	for _, bad := range []configuration{
		func() configuration { c := base; c.KeyFile = ""; c.KeyPasswordEnv = "PASSWORD"; return c }(),
		func() configuration { c := base; c.KeyPasswordEnv = "lowercase"; return c }(),
		func() configuration { c := base; c.KeyPasswordEnv = "SYRNCK_KEY"; return c }(),
		func() configuration { c := base; c.KeyPasswordEnv = "SIGNER"; c.SigningKeyEnv = "SIGNER"; return c }(),
	} {
		write(bad)
		if _, err := loadConfiguration(path); err == nil {
			t.Fatal("invalid provider accepted", bad)
		}
	}
}

func TestProtectedStorageKeyFailuresDoNotCapture(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, "work")
	if err := os.Mkdir(directory, 0700); err != nil {
		t.Fatal(err)
	}
	certificate, private, _ := testIdentity(t, root)
	_, _, peer := testIdentity(t, t.TempDir())
	config := configuration{Version: 1, Directory: directory, Storage: filepath.Join(directory, ".syrnck"), Folder: "project", Certificate: certificate, PrivateKey: private, Peer: peer, Mode: "listen", Address: "127.0.0.1:0", KeyEnv: "SYRNCK_BEP_FILE_RAW_TEST", KeyPasswordEnv: "SYRNCK_BEP_FILE_PASSWORD_TEST", KeyFile: filepath.Join(root, "storage.syrkey")}
	password := "provider-test-🔑"
	raw := bytes.Repeat([]byte{119}, 32)
	envelope, err := syrnck.ProtectKey(raw, []byte(password), syrnck.PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	store, err := syrnck.Open(config.Storage, syrnck.Options{Create: true, Key: raw})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	if err := os.WriteFile(filepath.Join(directory, "asset.bin"), []byte("not captured on failed authentication"), 0600); err != nil {
		t.Fatal(err)
	}
	writeKey := func(data []byte) {
		t.Helper()
		if err := os.WriteFile(config.KeyFile, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	writeKey(envelope)
	t.Setenv(config.KeyEnv, "")
	t.Setenv(config.KeyPasswordEnv, password)
	for _, name := range []string{"missing-password", "oversize-password", "wrong-password", "raw-conflict", "oversize", "tamper", "working-tree", "missing-file"} {
		t.Run(name, func(t *testing.T) {
			selected := config
			writeKey(envelope)
			switch name {
			case "missing-password":
				t.Setenv(config.KeyPasswordEnv, "")
			case "oversize-password":
				t.Setenv(config.KeyPasswordEnv, strings.Repeat("p", 4097))
			case "wrong-password":
				t.Setenv(config.KeyPasswordEnv, "wrong")
			case "raw-conflict":
				t.Setenv(config.KeyEnv, "not-a-key")
			case "oversize":
				writeKey(append(append([]byte{}, envelope...), 0))
			case "tamper":
				damaged := append([]byte{}, envelope...)
				damaged[100] ^= 1
				writeKey(damaged)
			case "working-tree":
				selected.KeyFile = filepath.Join(directory, "must-not-capture.syrkey")
				if err := os.WriteFile(selected.KeyFile, envelope, 0600); err != nil {
					t.Fatal(err)
				}
				defer os.Remove(selected.KeyFile)
			case "missing-file":
				selected.KeyFile += ".missing"
			}
			var output bytes.Buffer
			err := runService(context.Background(), selected, &output)
			if err == nil || output.Len() != 0 || strings.Contains(err.Error(), password) {
				t.Fatal("failed authentication started service or exposed password", err)
			}
		})
	}
	writeKey(envelope)
	t.Run("symlink", func(t *testing.T) {
		linked := config
		linked.KeyFile += ".link"
		if err := os.Symlink(config.KeyFile, linked.KeyFile); err != nil {
			t.Skip("symlink unavailable", err)
		}
		if _, err := loadStorageKey(linked, directory, config.Storage); err == nil {
			t.Fatal("symlink envelope accepted")
		}
	})
	store, err = syrnck.Open(config.Storage, syrnck.Options{Key: raw})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	refs, err := store.Refs()
	if err != nil || len(refs) != 0 {
		t.Fatal("ref changed on rejected provider", refs, err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	owner, err := loadStorageKey(config, directory, config.Storage)
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close()
	// An already authenticated resident owner survives removal of its envelope
	// and password. Reload/re-authentication is a process restart operation.
	if err := os.Remove(config.KeyFile); err != nil {
		t.Fatal(err)
	}
	t.Setenv(config.KeyPasswordEnv, "")
	opened, err := owner.Open(config.Storage, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := opened.Close(); err != nil {
		t.Fatal(err)
	}
	if translated := humanError("fr", errors.New("key password environment variable must contain 1..4096 bytes")); !strings.Contains(translated, "mot de passe") {
		t.Fatal(translated)
	}
}
