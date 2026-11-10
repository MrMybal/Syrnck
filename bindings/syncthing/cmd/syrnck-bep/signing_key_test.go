package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	syrnck "syrnck.local/bindings/go"
	"testing"
)

func TestProtectedSigningConfiguration(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "service.json")
	base := configuration{Version: 1, Directory: "work", Folder: "project", Certificate: "cert", PrivateKey: "private", Mode: "listen", Address: "127.0.0.1:0", SigningKeyFile: "author.syrsig", Author: &syrnck.NativeIdentity{Name: "Author", Email: "author@example.invalid"}}
	write := func(c configuration) {
		t.Helper()
		data, err := json.Marshal(c)
		if err != nil {
			t.Fatal(err)
		}
		if err = os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	write(base)
	loaded, err := loadConfiguration(path)
	if err != nil || loaded.SigningKeyFile != filepath.Join(root, "author.syrsig") || loaded.SigningKeyPasswordEnv != "SYRNCK_SIGNING_KEY_PASSWORD" {
		t.Fatal("protected signer defaults", loaded, err)
	}
	for _, bad := range []configuration{
		func() configuration { c := base; c.Author = nil; return c }(),
		func() configuration { c := base; c.SigningKeyEnv = "SIGNING"; return c }(),
		func() configuration { c := base; c.SigningKeyPasswordEnv = "bad-name"; return c }(),
		func() configuration { c := base; c.SigningKeyFile = ""; c.SigningKeyPasswordEnv = "PASSWORD"; return c }(),
		func() configuration { c := base; c.SigningKeyPasswordEnv = "SYRNCK_KEY"; return c }(),
		func() configuration {
			c := base
			c.KeyFile = "storage"
			c.KeyPasswordEnv = "PASSWORD"
			c.SigningKeyPasswordEnv = "PASSWORD"
			return c
		}(),
	} {
		write(bad)
		if _, err := loadConfiguration(path); err == nil {
			t.Fatal("invalid signing provider accepted", bad)
		}
	}
}

func TestProtectedSigningStartupAndNativeOwnership(t *testing.T) {
	root := testDirectory(t)
	directory := filepath.Join(root, "work")
	storage := filepath.Join(directory, ".syrnck")
	if err := os.Mkdir(directory, 0700); err != nil {
		t.Fatal(err)
	}
	certificate, private, _ := testIdentity(t, root)
	_, _, peer := testIdentity(t, t.TempDir())
	config := configuration{Version: 1, Directory: directory, Storage: storage, Folder: "project", Certificate: certificate, PrivateKey: private, Peer: peer, Mode: "listen", Address: "127.0.0.1:0", SigningKeyFile: filepath.Join(root, "author.syrsig"), SigningKeyPasswordEnv: "SYRNCK_BEP_SIGNING_PASSWORD_TEST", Author: &syrnck.NativeIdentity{Name: "Author", Email: "author@example.invalid"}}
	seed, password := bytes.Repeat([]byte{31}, 32), "signature credential 🔐"
	envelope, err := syrnck.ProtectSigningKey(seed, []byte(password), syrnck.PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	write := func(data []byte) {
		t.Helper()
		if err := os.WriteFile(config.SigningKeyFile, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	store, err := syrnck.Open(storage, syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	if err := os.WriteFile(filepath.Join(directory, "not-captured"), []byte("pending"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"missing-password", "bad-password", "long-password", "tamper", "wrong-purpose", "working-tree", "missing-file", "raw-conflict"} {
		t.Run(name, func(t *testing.T) {
			write(envelope)
			t.Setenv(config.SigningKeyPasswordEnv, password)
			selected := config
			switch name {
			case "missing-password":
				t.Setenv(config.SigningKeyPasswordEnv, "")
			case "bad-password":
				t.Setenv(config.SigningKeyPasswordEnv, "wrong")
			case "long-password":
				t.Setenv(config.SigningKeyPasswordEnv, strings.Repeat("p", 4097))
			case "tamper":
				bad := append([]byte{}, envelope...)
				bad[88] ^= 1
				write(bad)
			case "wrong-purpose":
				other, err := syrnck.ProtectKey(seed, []byte(password), syrnck.PasswordKdfOptions{})
				if err != nil {
					t.Fatal(err)
				}
				write(other)
			case "working-tree":
				selected.SigningKeyFile = filepath.Join(directory, "secret")
				if err := os.WriteFile(selected.SigningKeyFile, envelope, 0600); err != nil {
					t.Fatal(err)
				}
				defer os.Remove(selected.SigningKeyFile)
			case "missing-file":
				selected.SigningKeyFile += ".absent"
			case "raw-conflict":
				selected.SigningKeyEnv = "SIGNING"
			}
			var output bytes.Buffer
			err := runService(context.Background(), selected, &output)
			if err == nil || output.Len() != 0 || strings.Contains(err.Error(), password) {
				t.Fatal("invalid signing provider started workers or exposed credentials", err)
			}
		})
	}
	write(envelope)
	t.Setenv(config.SigningKeyPasswordEnv, password)
	owner, err := loadSigningKey(config, directory, storage)
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close()
	if _, present := os.LookupEnv(config.SigningKeyPasswordEnv); present {
		t.Fatal("signing password remained in service environment")
	}
	if err := os.Remove(config.SigningKeyFile); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		store, err := syrnck.Open(storage, syrnck.Options{})
		if err != nil {
			t.Fatal(err)
		}
		if err := store.ConfigureNativeIdentity(*config.Author, syrnck.NativeIdentityOptions{Signer: owner, RequireSignature: true}); err != nil {
			t.Fatal(err)
		}
		_, err = store.PrepareNativeRevision(json.RawMessage(`{"version":1,"parents":[],"message":"protected service signer","timestamp":7,"files":{}}`))
		if err != nil {
			t.Fatal("resident native owner failed after file/password removal", err)
		}
		refs, err := store.Refs()
		if err != nil || len(refs) != 0 {
			t.Fatal("failed startup captured a workspace", err, refs)
		}
		store.Close()
	}
	if value := humanError("fr", fmt.Errorf("signing key password environment variable must contain 1..4096 bytes")); !strings.Contains(value, "mot de passe") {
		t.Fatal(value)
	}
}
