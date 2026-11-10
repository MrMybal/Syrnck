package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	syrnck "syrnck.local/bindings/go"
)

func TestFolderPasswordConfigurationAndOneTimeProvider(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "service.json")
	base := configuration{Version: 1, Directory: "work", Folder: "project", Certificate: "cert.pem",
		PrivateKey: "device.pem", Mode: "listen", Address: "127.0.0.1:0", UntrustedPeer: true}
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
	if err != nil || loaded.FolderPasswordEnv != "SYRNCK_BEP_FOLDER_PASSWORD" {
		t.Fatal("default folder provider", err)
	}
	for _, bad := range []configuration{
		func() configuration { c := base; c.UntrustedPeer = false; c.FolderPasswordEnv = "PASSWORD"; return c }(),
		func() configuration { c := base; c.FolderPasswordEnv = "lowercase"; return c }(),
		func() configuration { c := base; c.FolderPasswordEnv = "SYRNCK_KEY"; return c }(),
		func() configuration { c := base; c.FolderPasswordEnv = "SIGNER"; c.SigningKeyEnv = "SIGNER"; return c }(),
		func() configuration { c := base; c.FolderPasswordEnv = "SYRNCK_SEALED_KEY"; return c }(),
	} {
		write(bad)
		if _, err := loadConfiguration(path); err == nil {
			t.Fatal("invalid folder credential configuration accepted")
		}
	}
	for _, password := range []string{"", strings.Repeat("x", 4097), "test-password 密钥"} {
		t.Setenv(loaded.FolderPasswordEnv, password)
		value, err := loadFolderPassword(loaded)
		if (err == nil) != (len(password) > 0 && len(password) <= 4096) {
			t.Fatal("password validation", err)
		}
		if err == nil && value != password {
			t.Fatal("password changed")
		}
		if _, found := os.LookupEnv(loaded.FolderPasswordEnv); found {
			t.Fatal("consumed folder password stayed in environment")
		}
	}
	_, missing := loadFolderPassword(loaded)
	if missing == nil {
		t.Fatal("missing credential accepted")
	}
	if text := humanError("fr", missing); !strings.Contains(text, "mot de passe") {
		t.Fatal("folder diagnostic untranslated", text)
	}
}

func TestMissingFolderPasswordDoesNotCaptureOrListen(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, "work")
	if err := os.Mkdir(directory, 0700); err != nil {
		t.Fatal(err)
	}
	cert, key, _ := testIdentity(t, root)
	_, _, peer := testIdentity(t, t.TempDir())
	storage := filepath.Join(directory, ".syrnck")
	store, err := syrnck.Open(storage, syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	if err := os.WriteFile(filepath.Join(directory, "private.txt"), []byte("not yet captured"), 0600); err != nil {
		t.Fatal(err)
	}
	config := configuration{Version: 1, Directory: directory, Storage: storage, Folder: "project", Certificate: cert,
		PrivateKey: key, Peer: peer, Mode: "listen", Address: "127.0.0.1:0", KeyEnv: "SYRNCK_TEST_EMPTY_STORAGE_KEY",
		UntrustedPeer: true, FolderPasswordEnv: "SYRNCK_TEST_EMPTY_FOLDER_PASSWORD"}
	t.Setenv(config.KeyEnv, "")
	t.Setenv(config.FolderPasswordEnv, "")
	var output bytes.Buffer
	if err := runService(context.Background(), config, &output); err == nil {
		t.Fatal("missing untrusted credential started service")
	}
	if output.Len() != 0 {
		t.Fatal("failed service reported ready")
	}
	store, err = syrnck.Open(storage, syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	refs, err := store.Refs()
	if err != nil || len(refs) != 0 {
		t.Fatal("invalid credential captured a native root", refs, err)
	}
}
