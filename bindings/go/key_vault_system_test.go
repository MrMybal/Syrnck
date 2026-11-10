package syrnck

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"os"
	"path/filepath"
	"testing"
)

var vaultFixtureMode = flag.String("syrnck-vault-mode", "", "owned real-vault fixture phase")
var vaultFixtureRoot = flag.String("syrnck-vault-root", "", "owned real-vault fixture directory")
var vaultFixtureKeychain = flag.String("syrnck-vault-keychain", "", "selected disposable macOS keychain")

type vaultFixtureState struct {
	References []VaultReference `json:"references"`
	Object     ObjectKey        `json:"object"`
}

// Called explicitly by key_vault_system.py in a separately built Go test
// binary. Default library tests never read or mutate a developer's OS vault.
func TestSystemVaultRealProvider(t *testing.T) {
	if *vaultFixtureMode == "" {
		t.Skip("explicit disposable system-vault fixture only")
	}
	root := *vaultFixtureRoot
	if !filepath.IsAbs(root) {
		t.Fatal("fixture root must be absolute")
	}
	if err := DisableMacOSKeychainInteraction(); err != nil {
		t.Fatal(err)
	}
	vault, err := OpenSystemKeyVault(*vaultFixtureKeychain)
	if err != nil {
		t.Fatal(err)
	}
	defer vault.Close()
	provider, err := vault.Provider()
	if err != nil || !provider.Available {
		t.Fatal("real provider unavailable", err)
	}
	password := bytes.Repeat([]byte{29}, 32) // Disposable synthetic fixture only.
	statePath := filepath.Join(root, "state.json")
	var state vaultFixtureState
	if *vaultFixtureMode != "create" {
		data, err := os.ReadFile(statePath)
		if err != nil {
			t.Fatal(err)
		}
		if err := json.Unmarshal(data, &state); err != nil {
			t.Fatal(err)
		}
	}
	save := func() {
		data, err := json.Marshal(state)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(statePath, data, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	switch *vaultFixtureMode {
	case "create":
		state.References = make([]VaultReference, 0, 3)
		complete := false
		defer func() {
			if !complete {
				for _, ref := range state.References {
					if _, err := vault.Erase(ref); err != nil {
						t.Error(err)
					}
				}
			}
		}()
		for purpose := VaultStorageKey; purpose <= VaultSealedKey; purpose++ {
			ref, err := vault.Generate(purpose)
			if err != nil {
				t.Fatal(err)
			}
			state.References = append(state.References, ref)
			save()
			envelope, err := vault.ExportProtected(ref, password, PasswordKdfOptions{})
			if err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(root, string(rune('0'+purpose))+".envelope"), envelope, 0o600); err != nil {
				t.Fatal(err)
			}
		}
		complete = true
	case "verify":
		if len(state.References) != 3 {
			t.Fatal("missing Go fixture references")
		}
		for index, original := range state.References {
			purpose := VaultKeyPurpose(index + 1)
			envelope, err := os.ReadFile(filepath.Join(root, string(rune('1'+index))+".envelope"))
			if err != nil {
				t.Fatal(err)
			}
			if _, err := vault.ImportProtected(purpose, envelope, []byte("wrong synthetic password")); err == nil {
				t.Fatal("wrong import password accepted")
			}
			ref, err := vault.ImportProtected(purpose, envelope, password)
			if err != nil {
				t.Fatal(err)
			}
			func() {
				defer func() {
					if _, err := vault.Erase(ref); err != nil {
						t.Error(err)
					}
				}()
				if ref == original {
					t.Fatal("import reused a vault reference")
				}
				data, err := vault.ExportProtected(ref, password, PasswordKdfOptions{})
				if err != nil || len(data) != ProtectedKeySize || bytes.Equal(data, envelope) {
					t.Fatal("invalid fresh protected export", err)
				}
				if purpose == VaultSigningKey {
					first, err := vault.OpenSigner(original)
					if err != nil {
						t.Fatal(err)
					}
					defer first.Close()
					second, err := vault.OpenSigner(ref)
					if err != nil {
						t.Fatal(err)
					}
					defer second.Close()
					a, err := first.PublicMetadata()
					if err != nil {
						t.Fatal(err)
					}
					b, err := second.PublicMetadata()
					if err != nil || !bytes.Equal(a, b) {
						t.Fatal("signer import changed identity", err)
					}
				} else if purpose == VaultSealedKey {
					first, err := vault.OpenSealed(original)
					if err != nil {
						t.Fatal(err)
					}
					defer first.Close()
					second, err := vault.OpenSealed(ref)
					if err != nil {
						t.Fatal(err)
					}
					defer second.Close()
					a, err := first.PublicMetadata()
					if err != nil {
						t.Fatal(err)
					}
					b, err := second.PublicMetadata()
					if err != nil || !bytes.Equal(a, b) {
						t.Fatal("master import changed realm", err)
					}
				}
			}()
		}
		key, err := vault.OpenSecret(state.References[0])
		if err != nil {
			t.Fatal(err)
		}
		defer key.Close()
		signer, err := vault.OpenSigner(state.References[1])
		if err != nil {
			t.Fatal(err)
		}
		defer signer.Close()
		master, err := vault.OpenSealed(state.References[2])
		if err != nil {
			t.Fatal(err)
		}
		defer master.Close()
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
		if err := vault.Close(); err != nil {
			t.Fatal(err)
		}
		store, err := key.Open(filepath.Join(root, "store"), true)
		if err != nil {
			t.Fatal(err)
		}
		defer store.Close()
		if err := key.Close(); err != nil {
			t.Fatal(err)
		}
		if err := store.ConfigureNativeIdentity(NativeIdentity{Name: "Vault Go", Email: "vault-go@example.invalid"}, NativeIdentityOptions{Signer: signer, RequireSignature: true}); err != nil {
			t.Fatal(err)
		}
		if err := signer.Close(); err != nil {
			t.Fatal(err)
		}
		revision, err := store.PrepareNativeRevision(json.RawMessage(`{"version":1,"parents":[],"message":"vault Go fixture","timestamp":9,"files":{}}`))
		if err != nil {
			t.Fatal(err)
		}
		signed, err := store.Get(revision)
		if err != nil || !bytes.Contains(signed, []byte(identity.PublicKey)) {
			t.Fatal("native Go vault owner did not sign", err)
		}
		data := []byte("synthetic Go vault sealed snapshot")
		state.Object, err = store.Put("chunk", data)
		if err != nil {
			t.Fatal(err)
		}
		wire, err := Open(filepath.Join(root, "wire"), Options{Create: true})
		if err != nil {
			t.Fatal(err)
		}
		defer wire.Close()
		destination, err := Open(filepath.Join(root, "destination"), Options{Create: true})
		if err != nil {
			t.Fatal(err)
		}
		defer destination.Close()
		sealed, err := master.SealSnapshot(context.Background(), store, wire, "vault-go", map[string]ObjectKey{"fixture/object": state.Object}, "", SealedLimits{})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := master.OpenSnapshot(context.Background(), wire, destination, sealed.Snapshot, "vault-go", "sealed/checkpoints/vault-go", SealedLimits{}); err != nil {
			t.Fatal(err)
		}
		got, err := destination.Get(state.Object)
		if err != nil || !bytes.Equal(got, data) {
			t.Fatal("Go vault master reconstruction", err)
		}
		if err := store.Close(); err != nil {
			t.Fatal(err)
		}
		envelope, err := os.ReadFile(filepath.Join(root, "1.envelope"))
		if err != nil {
			t.Fatal(err)
		}
		backup, err := OpenProtectedKey(envelope, password)
		if err != nil {
			t.Fatal(err)
		}
		defer backup.Close()
		reopened, err := backup.Open(filepath.Join(root, "store"), false)
		if err != nil {
			t.Fatal(err)
		}
		defer reopened.Close()
		got, err = reopened.Get(state.Object)
		if err != nil || !bytes.Equal(got, data) {
			t.Fatal("Go vault protected storage backup", err)
		}
		save()
	case "erase":
		key, err := vault.OpenSecret(state.References[0])
		if err != nil {
			t.Fatal(err)
		}
		defer key.Close()
		signer, err := vault.OpenSigner(state.References[1])
		if err != nil {
			t.Fatal(err)
		}
		defer signer.Close()
		master, err := vault.OpenSealed(state.References[2])
		if err != nil {
			t.Fatal(err)
		}
		defer master.Close()
		for _, ref := range state.References {
			if removed, err := vault.Erase(ref); err != nil || !removed {
				t.Fatal("erase", err)
			}
			if removed, err := vault.Erase(ref); err != nil || removed {
				t.Fatal("duplicate erase", err)
			}
			if _, err := vault.ExportProtected(ref, password, PasswordKdfOptions{}); err == nil {
				t.Fatal("erased entry reopened")
			}
		}
		if _, err := signer.PublicMetadata(); err != nil {
			t.Fatal(err)
		}
		if _, err := master.PublicMetadata(); err != nil {
			t.Fatal(err)
		}
		store, err := key.Open(filepath.Join(root, "store"), false)
		if err != nil {
			t.Fatal(err)
		}
		defer store.Close()
		if _, err := store.Get(state.Object); err != nil {
			t.Fatal(err)
		}
	case "cleanup":
		for _, ref := range state.References {
			if _, err := vault.Erase(ref); err != nil {
				t.Fatal(err)
			}
		}
	case "inaccessible":
		for _, ref := range state.References {
			if _, err := vault.ExportProtected(ref, password, PasswordKdfOptions{}); err == nil {
				t.Fatal("inaccessible vault entry was read")
			}
		}
	default:
		t.Fatal("unknown real-vault fixture phase")
	}
}
