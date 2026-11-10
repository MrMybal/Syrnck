package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	syrnck "syrnck.local/bindings/go"
)

var vaultFixtureMode = flag.String("syrnck-vault-mode", "", "owned real-vault fixture phase")
var vaultFixtureRoot = flag.String("syrnck-vault-root", "", "owned real-vault fixture directory")
var vaultFixtureKeychain = flag.String("syrnck-vault-keychain", "", "selected disposable macOS keychain")

type vaultReadyWriter struct{ ready chan string }

func (w vaultReadyWriter) Write(data []byte) (int, error) {
	var event struct{ Event, Address string }
	if err := json.Unmarshal(data, &event); err != nil {
		return 0, err
	}
	if event.Event == "ready" {
		w.ready <- event.Address
	}
	return len(data), nil
}

// The outer OS fixture opts in explicitly and creates a separate test process
// for every phase. Ordinary Go tests never use a developer's account vault.
func TestSystemVaultRealProvider(t *testing.T) {
	if *vaultFixtureMode == "" {
		t.Skip("explicit disposable system-vault fixture only")
	}
	root := *vaultFixtureRoot
	if !filepath.IsAbs(root) {
		t.Fatal("fixture root must be absolute")
	}
	statePath := filepath.Join(root, "state.json")
	var refs []syrnck.VaultReference
	if *vaultFixtureMode != "create" {
		data, err := os.ReadFile(statePath)
		if err != nil {
			t.Fatal(err)
		}
		if err := json.Unmarshal(data, &refs); err != nil {
			t.Fatal(err)
		}
	}
	config := configuration{Version: 1, Directory: filepath.Join(root, "work"), Storage: filepath.Join(root, "work", ".syrnck"), Folder: "project",
		Mode: "listen", Address: "127.0.0.1:0", KeyEnv: "SYRNCK_BEP_VAULT_FIXTURE_RAW", VaultKeychain: *vaultFixtureKeychain,
		Author: &syrnck.NativeIdentity{Name: "Vault service", Email: "vault-service@example.invalid"}}
	// Configuration alone owns no keys. The private fixture enrolls its own
	// entries through the same executable that will consume them on macOS.
	vault, err := openSelectedVault(config)
	if err != nil {
		t.Fatal(err)
	}
	defer vault.Close()
	if *vaultFixtureMode == "create" {
		save := func() {
			data, err := json.Marshal(refs)
			if err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(statePath, data, 0600); err != nil {
				t.Fatal(err)
			}
		}
		save()
		for _, purpose := range []syrnck.VaultKeyPurpose{syrnck.VaultStorageKey, syrnck.VaultSigningKey} {
			ref, err := vault.Generate(purpose)
			if err != nil {
				t.Fatal(err)
			}
			refs = append(refs, ref)
			save()
		}
		if err := os.Mkdir(config.Directory, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(config.Directory, "asset.bin"), []byte("offline vault-backed service asset"), 0600); err != nil {
			t.Fatal(err)
		}
		return
	}
	if *vaultFixtureMode == "cleanup" {
		for _, ref := range refs {
			if _, err := vault.Erase(ref); err != nil {
				t.Fatal(err)
			}
		}
		return
	}
	if len(refs) != 2 {
		t.Fatal("missing fixture references")
	}
	config.KeyVault, config.SigningKeyVault = refs[0].String(), refs[1].String()
	if *vaultFixtureMode == "inaccessible" {
		if key, err := loadStorageKey(config, "", ""); err == nil {
			key.Close()
			t.Fatal("locked storage entry opened")
		}
		if key, err := loadSigningKey(config, "", ""); err == nil {
			key.Close()
			t.Fatal("locked signing entry opened")
		}
		return
	}
	key, err := loadStorageKey(config, "", "")
	if err != nil {
		t.Fatal(err)
	}
	defer key.Close()
	signer, err := loadSigningKey(config, "", "")
	if err != nil {
		t.Fatal(err)
	}
	defer signer.Close()
	public, err := signer.PublicMetadata()
	if err != nil {
		t.Fatal(err)
	}
	var identity struct {
		PublicKey string `json:"public_key"`
	}
	if err := json.Unmarshal(public, &identity); err != nil {
		t.Fatal(err)
	}
	if *vaultFixtureMode == "verify" {
		store, err := key.Open(config.Storage, true)
		if err != nil {
			t.Fatal(err)
		}
		if err := store.Close(); err != nil {
			t.Fatal(err)
		}
		identityRoot := filepath.Join(root, "identity")
		if err := os.Mkdir(identityRoot, 0700); err != nil {
			t.Fatal(err)
		}
		config.Certificate, config.PrivateKey, _ = testIdentity(t, identityRoot)
		_, _, config.Peer = testIdentity(t, t.TempDir())
		encoded, err := json.Marshal(config)
		if err != nil {
			t.Fatal(err)
		}
		configurationPath := filepath.Join(root, "service.json")
		if err := os.WriteFile(configurationPath, encoded, 0600); err != nil {
			t.Fatal(err)
		}
		config, err = loadConfiguration(configurationPath)
		if err != nil {
			t.Fatal(err)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
		defer cancel()
		writer := vaultReadyWriter{ready: make(chan string, 1)}
		done := make(chan error, 1)
		go func() { done <- runService(ctx, config, writer) }()
		select {
		case address := <-writer.ready:
			if strings.HasSuffix(address, ":0") {
				cancel()
				<-done
				t.Fatal("listener address not bound")
			}
		case err := <-done:
			t.Fatal("vault service stopped before ready", err)
		case <-ctx.Done():
			cancel()
			<-done
			t.Fatal("vault service readiness timed out")
		}
		cancel()
		select {
		case err := <-done:
			if err != nil {
				t.Fatal(err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("vault service leaked workers")
		}
		store, err = key.Open(config.Storage, false)
		if err != nil {
			t.Fatal(err)
		}
		defer store.Close()
		roots, err := store.Refs()
		if err != nil {
			t.Fatal(err)
		}
		revision, ok := roots["heads/main"]
		if !ok {
			t.Fatal("vault service did not capture offline asset")
		}
		data, err := store.Get(revision)
		if err != nil || !bytes.Contains(data, []byte(identity.PublicKey)) || !bytes.Contains(data, []byte("asset.bin")) {
			t.Fatal("vault service omitted signature or asset", err)
		}
		return
	}
	if *vaultFixtureMode != "erase" {
		t.Fatal("unexpected fixture phase")
	}
	for _, ref := range refs {
		removed, err := vault.Erase(ref)
		if err != nil || !removed {
			t.Fatal("fixture entry not erased", err)
		}
	}
	if other, err := loadStorageKey(config, "", ""); err == nil {
		other.Close()
		t.Fatal("erased entry reopened")
	}
	if other, err := loadSigningKey(config, "", ""); err == nil {
		other.Close()
		t.Fatal("erased signer reopened")
	}
	// Active native owners retain their authenticated key material until close.
	store, err := key.Open(config.Storage, false)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if err := store.ConfigureNativeIdentity(*config.Author, syrnck.NativeIdentityOptions{Signer: signer, RequireSignature: true}); err != nil {
		t.Fatal(err)
	}
	prepared, err := store.PrepareNativeRevision(json.RawMessage(`{"version":1,"parents":[],"message":"retained service owner","timestamp":3,"files":{}}`))
	if err != nil {
		t.Fatal(err)
	}
	data, err := store.Get(prepared)
	if err != nil || !bytes.Contains(data, []byte(identity.PublicKey)) {
		t.Fatal("retained signer changed", err)
	}
}
