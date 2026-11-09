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

func TestAuthorConfigurationAndMissingRequiredSigningKey(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, "work")
	if err := os.Mkdir(directory, 0700); err != nil {
		t.Fatal(err)
	}
	certificate, key, _ := testIdentity(t, root)
	_, _, peer := testIdentity(t, t.TempDir())
	storage := filepath.Join(directory, ".syrnck")
	store, err := syrnck.Open(storage, syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	config := configuration{Version: 1, Directory: directory, Storage: storage, Folder: "project", Certificate: certificate, PrivateKey: key, Peer: peer, Mode: "listen", Address: "127.0.0.1:0", KeyEnv: "SYRNCK_BEP_REQUIRED_STORAGE_TEST", Author: &syrnck.NativeIdentity{Name: "Author", Email: "author@example.invalid", TimezoneOffset: -120}, SigningKeyEnv: "SYRNCK_BEP_REQUIRED_SIGNING_TEST"}
	t.Setenv(config.KeyEnv, "")
	t.Setenv(config.SigningKeyEnv, "")
	path := filepath.Join(root, "service.json")
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
	write(config)
	loaded, err := loadConfiguration(path)
	if err != nil || loaded.Author == nil || loaded.Author.TimezoneOffset != -120 {
		t.Fatal("author configuration did not round trip", err)
	}
	for _, bad := range []configuration{
		func() configuration { bad := config; bad.Author = nil; return bad }(),
		func() configuration { bad := config; bad.SigningKeyEnv = "lowercase"; return bad }(),
		func() configuration { bad := config; bad.KeyEnv = config.SigningKeyEnv; return bad }(),
		func() configuration {
			bad := config
			bad.Author = &syrnck.NativeIdentity{Name: "Author", Email: "email", TimezoneOffset: 1440}
			return bad
		}(),
	} {
		write(bad)
		if _, err := loadConfiguration(path); err == nil {
			t.Fatal("invalid author/key configuration accepted")
		}
	}
	write(config)
	var output, diagnostics bytes.Buffer
	if code := execute(context.Background(), []string{"run", "--config", path}, &output, &diagnostics); code == 0 || !strings.Contains(diagnostics.String(), "native signing key is required") || output.Len() != 0 {
		t.Fatal("missing mandatory signing seed silently produced a snapshot", code, diagnostics.String())
	}
	output.Reset()
	diagnostics.Reset()
	if code := execute(context.Background(), []string{"--lang", "fr", "run", "--config", path}, &output, &diagnostics); code == 0 || !strings.Contains(diagnostics.String(), "clé de signature native est requise") || output.Len() != 0 {
		t.Fatal("French required-signing diagnostic missing", code, diagnostics.String())
	}
	store, err = syrnck.Open(storage, syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	refs, err := store.Refs()
	if err != nil {
		t.Fatal(err)
	}
	if _, exists := refs["heads/main"]; exists {
		t.Fatal("missing required signing key advanced HEAD")
	}
}
