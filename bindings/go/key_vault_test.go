package syrnck

import (
	"errors"
	"strings"
	"sync"
	"testing"
)

func TestVaultReferenceValidation(t *testing.T) {
	for index, name := range []string{"storage", "signing", "sealed"} {
		value := "syrnck-vault:1:" + name + ":" + strings.Repeat("a", 64)
		ref, err := ParseVaultReference(value)
		if err != nil || ref.String() != value {
			t.Fatal("reference round trip", err)
		}
		purpose, err := ref.Purpose()
		if err != nil || purpose != VaultKeyPurpose(index+1) {
			t.Fatal("reference purpose", err)
		}
		for _, bad := range []string{"", value + "\n", " " + value, value + "0", value[:len(value)-1],
			value[:len(value)-1] + "A", value[:len(value)-1] + "/", value[:len(value)-1] + "\x00",
			strings.Replace(value, ":1:", ":2:", 1), "syrnck-vault:1:storage:" + strings.Repeat("a", 1024*1024)} {
			if parsed, err := ParseVaultReference(bad); err == nil || parsed != "" {
				t.Fatal("accepted invalid reference")
			}
		}
	}
}

func TestSystemVaultConfigurationAndLifetime(t *testing.T) {
	if _, err := OpenSystemKeyVault("relative.keychain"); err == nil {
		t.Fatal("accepted relative keychain")
	}
	if _, err := OpenSystemKeyVault("bad\x00keychain"); err == nil {
		t.Fatal("accepted embedded NUL")
	}
	vault, err := OpenSystemKeyVault("")
	if err != nil {
		t.Fatal(err)
	}
	defer vault.Close()
	provider, err := vault.Provider()
	if err != nil || provider.Version != 1 || provider.Provider == "" {
		t.Fatal(provider, err)
	}
	ref := VaultReference("syrnck-vault:1:storage:" + strings.Repeat("a", 64))
	if _, err := vault.Generate(0); err == nil {
		t.Fatal("accepted unknown purpose")
	}
	if _, err := vault.Generate(4); err == nil {
		t.Fatal("accepted unknown purpose")
	}
	if _, err := vault.OpenSigner(ref); err == nil {
		t.Fatal("storage key accepted for signing")
	}
	if _, err := vault.OpenSealed(ref); err == nil {
		t.Fatal("storage key accepted for sealing")
	}
	if _, err := vault.ImportProtected(VaultStorageKey, nil, []byte("fixture")); err == nil {
		t.Fatal("accepted missing envelope")
	}
	if _, err := vault.ImportProtected(VaultStorageKey, make([]byte, ProtectedKeySize), nil); err == nil {
		t.Fatal("accepted empty password")
	}
	if _, err := vault.ExportProtected(ref, nil, PasswordKdfOptions{}); err == nil {
		t.Fatal("accepted empty password")
	}
	if _, err := vault.Erase("*"); err == nil {
		t.Fatal("accepted wildcard reference")
	}
	// Real-provider calls are deliberately opt-in. These default tests must
	// never create credentials or read a developer's existing account vault.
	if !provider.Available {
		if _, err := vault.Generate(VaultStorageKey); err == nil {
			t.Fatal("unavailable provider fell back")
		}
	}
	if err := vault.Close(); err != nil {
		t.Fatal(err)
	}
	if err := vault.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := vault.Provider(); !errors.Is(err, ErrSystemKeyVaultClosed) {
		t.Fatal("closed context", err)
	}
	if _, err := vault.Generate(VaultStorageKey); !errors.Is(err, ErrSystemKeyVaultClosed) {
		t.Fatal("closed context", err)
	}
	if _, err := vault.OpenSecret(ref); !errors.Is(err, ErrSystemKeyVaultClosed) {
		t.Fatal("closed context", err)
	}
	if _, err := vault.Erase(ref); !errors.Is(err, ErrSystemKeyVaultClosed) {
		t.Fatal("closed context", err)
	}
	var missing *SystemKeyVault
	if err := missing.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := missing.Provider(); !errors.Is(err, ErrSystemKeyVaultClosed) {
		t.Fatal("nil context", err)
	}
}

func TestSystemVaultConcurrentConfigurationClose(t *testing.T) {
	vault, err := OpenSystemKeyVault("")
	if err != nil {
		t.Fatal(err)
	}
	var workers sync.WaitGroup
	for range 8 {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for range 16 {
				if _, err := vault.Provider(); err != nil && !errors.Is(err, ErrSystemKeyVaultClosed) {
					t.Error(err)
				}
			}
		}()
	}
	if err := vault.Close(); err != nil {
		t.Fatal(err)
	}
	workers.Wait()
}
