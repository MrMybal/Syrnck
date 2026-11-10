package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	syrnck "syrnck.local/bindings/go"
)

func TestVaultConfiguration(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "service.json")
	storage := "syrnck-vault:1:storage:" + strings.Repeat("0", 64)
	signing := "syrnck-vault:1:signing:" + strings.Repeat("1", 64)
	base := configuration{Version: 1, Directory: "work", Storage: "store", Folder: "project", Certificate: "cert", PrivateKey: "private",
		Mode: "listen", Address: "127.0.0.1:0", KeyVault: storage, SigningKeyVault: signing,
		Author: &syrnck.NativeIdentity{Name: "Vault author", Email: "vault@example.invalid"}}
	write := func(c configuration) {
		t.Helper()
		data, err := json.Marshal(c)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	write(base)
	loaded, err := loadConfiguration(path)
	if err != nil || loaded.KeyVault != storage || loaded.SigningKeyVault != signing || loaded.KeyPasswordEnv != "" || loaded.SigningKeyPasswordEnv != "" {
		t.Fatal("public vault selectors changed or acquired password defaults", loaded, err)
	}
	invalid := []configuration{
		func() configuration { c := base; c.KeyFile = "file"; return c }(),
		func() configuration { c := base; c.KeyVault = signing; return c }(),
		func() configuration { c := base; c.KeyVault += "\n"; return c }(),
		func() configuration { c := base; c.KeyPasswordEnv = "PASSWORD"; return c }(),
		func() configuration { c := base; c.SigningKeyVault = storage; return c }(),
		func() configuration { c := base; c.SigningKeyEnv = "SIGNER"; return c }(),
		func() configuration { c := base; c.SigningKeyFile = "file"; return c }(),
		func() configuration { c := base; c.SigningKeyPasswordEnv = "PASSWORD"; return c }(),
		func() configuration { c := base; c.Author = nil; return c }(),
		func() configuration { c := base; c.VaultKeychain = "relative.keychain"; return c }(),
		func() configuration {
			c := base
			c.KeyVault = ""
			c.SigningKeyVault = ""
			c.VaultKeychain = filepath.Join(root, "vault")
			return c
		}(),
		func() configuration {
			c := base
			c.UntrustedPeer = true
			c.FolderPasswordEnv = "SYRNCK_KEY_VAULT"
			return c
		}(),
		func() configuration { c := base; c.ReceiveEncrypted = true; c.Directory = ""; return c }(),
	}
	if runtime.GOOS != "darwin" {
		c := base
		c.VaultKeychain = filepath.Join(root, "vault")
		invalid = append(invalid, c)
	} else {
		c := base
		c.VaultKeychain = filepath.Join(root, "vault")
		write(c)
		if _, err := loadConfiguration(path); err != nil {
			t.Fatal("absolute selected keychain refused", err)
		}
	}
	for _, bad := range invalid {
		write(bad)
		if _, err := loadConfiguration(path); err == nil {
			t.Fatal("invalid vault configuration accepted", bad)
		}
	}
	for _, bad := range []configuration{invalid[0], invalid[1], invalid[2], invalid[4], invalid[5], invalid[6], invalid[8], invalid[9]} {
		var output bytes.Buffer
		if err := runService(context.Background(), bad, &output); err == nil || output.Len() != 0 {
			t.Fatal("invalid vault selection reached service startup", err)
		}
	}
	if _, err := os.Stat(filepath.Join(root, "store")); !os.IsNotExist(err) {
		t.Fatal("invalid configuration created Store", err)
	}
	for _, field := range []string{"key_vault", "signing_key_vault", "vault_keychain"} {
		for _, value := range []any{"", nil, false, 1} {
			write(base)
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			var members map[string]any
			if err := json.Unmarshal(data, &members); err != nil {
				t.Fatal(err)
			}
			members[field] = value
			data, err = json.Marshal(members)
			if err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(path, data, 0600); err != nil {
				t.Fatal(err)
			}
			if _, err := loadConfiguration(path); err == nil {
				t.Fatal("empty/null/typed selector silently chose a different provider", field, value)
			}
		}
		write(base)
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		data = bytes.Replace(data, []byte(`"`+field+`"`), []byte(`"`+strings.ToUpper(field)+`"`), 1)
		if field == "vault_keychain" {
			data = append(data[:len(data)-1], []byte(`,"VAULT_KEYCHAIN":null}`)...)
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := loadConfiguration(path); err == nil {
			t.Fatal("noncanonical selector alias accepted", field)
		}
	}
}

func TestVaultLoadersRejectAmbiguityBeforeAccountAccess(t *testing.T) {
	storage := "syrnck-vault:1:storage:" + strings.Repeat("0", 64)
	t.Setenv("SYRNCK_BEP_VAULT_RAW_TEST", "conflicting")
	c := configuration{KeyVault: storage, KeyEnv: "SYRNCK_BEP_VAULT_RAW_TEST"}
	if _, err := loadStorageKey(c, "", ""); err == nil || err.Error() != "key_vault conflicts with a nonempty raw storage key environment variable" {
		t.Fatal("raw key conflict reached account vault", err)
	}
	c.KeyVault = "syrnck-vault:1:sealed:" + strings.Repeat("0", 64)
	if _, err := loadStorageKey(c, "", ""); err == nil || err.Error() != "vault reference has another key purpose" {
		t.Fatal(err)
	}
	c = configuration{SigningKeyVault: storage, Author: &syrnck.NativeIdentity{Name: "Author", Email: "author@example.invalid"}}
	if _, err := loadSigningKey(c, "", ""); err == nil || err.Error() != "vault reference has another key purpose" {
		t.Fatal(err)
	}
	for _, message := range []string{"vault reference has another key purpose", "choose key_file or key_vault", "signing_key_vault requires an explicit author",
		"vault_keychain requires key_vault or signing_key_vault", "vault_keychain must name an absolute macOS keychain",
		"choose signing_key_env, signing_key_file or signing_key_vault", "key_vault conflicts with a nonempty raw storage key environment variable"} {
		if got := humanError("fr", errors.New(message)); got == message {
			t.Fatal("missing French diagnostic", message)
		}
	}
}
