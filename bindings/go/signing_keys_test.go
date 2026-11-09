package syrnck

import (
	"bytes"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"path/filepath"
	"testing"
)

func TestProtectedSignerOwnershipAndRewrap(t *testing.T) {
	seed := bytes.Repeat([]byte{41}, 32)
	expected := ed25519.NewKeyFromSeed(seed).Public().(ed25519.PublicKey)
	password, next := []byte("签名 credential"), []byte{'n', 0, 'w'}
	sealed, err := ProtectSigningKey(seed, password, PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	clear(seed)
	if bad, err := OpenProtectedSigner(sealed, []byte("incorrect")); err == nil || bad != nil {
		t.Fatal("bad password returned a signer", err)
	}
	if bad, err := OpenProtectedKey(sealed, password); err == nil || bad != nil {
		t.Fatal("signing envelope became a storage key", err)
	}
	owner, err := OpenProtectedSigner(sealed, password)
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close()
	metadata, err := owner.PublicMetadata()
	if err != nil || !bytes.Contains(metadata, []byte(hex.EncodeToString(expected))) {
		t.Fatal("public owner differs", err, string(metadata))
	}
	for _, encrypted := range []bool{false, true} {
		var storageKey []byte
		if encrypted {
			storageKey = bytes.Repeat([]byte{19}, 32)
		}
		store, err := Open(filepath.Join(tempDirectory(t), "store"), Options{Key: storageKey, Create: true})
		if err != nil {
			t.Fatal(err)
		}
		identity := NativeIdentity{Name: "Protected author", Email: "protected@example.invalid", TimezoneOffset: 60}
		if err := store.ConfigureNativeIdentity(identity, NativeIdentityOptions{Signer: owner, RequireSignature: true}); err != nil {
			t.Fatal(err)
		}
		before, err := store.NativeIdentityMetadata()
		if err != nil {
			t.Fatal(err)
		}
		if err := store.ConfigureNativeIdentity(identity, NativeIdentityOptions{Seed: bytes.Repeat([]byte{1}, 32), Signer: owner}); err == nil {
			t.Fatal("ambiguous providers accepted")
		}
		bad := identity
		bad.TimezoneOffset = 1440
		if err := store.ConfigureNativeIdentity(bad, NativeIdentityOptions{Signer: owner}); err == nil {
			t.Fatal("invalid protected identity accepted")
		}
		after, _ := store.NativeIdentityMetadata()
		if !bytes.Equal(before, after) {
			t.Fatal("failed protected config changed identity")
		}
		draft := json.RawMessage(`{"version":1,"parents":[],"message":"protected owner","timestamp":7,"files":{}}`)
		object, err := store.PrepareNativeRevision(draft)
		if err != nil {
			t.Fatal(err)
		}
		data, err := store.Get(object)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Contains(data, []byte(hex.EncodeToString(expected))) {
			t.Fatal("native owner signed with another key")
		}
		if err := store.Close(); err != nil {
			t.Fatal(err)
		}
	}
	changed, err := ChangeSigningKeyPassword(sealed, password, next, PasswordKdfOptions{Passes: 4})
	if err != nil {
		t.Fatal(err)
	}
	replacement, err := OpenProtectedSigner(changed, next)
	if err != nil {
		t.Fatal(err)
	}
	after, err := replacement.PublicMetadata()
	if err != nil || !bytes.Equal(metadata, after) {
		t.Fatal("rewrap changed signer", err)
	}
	store, err := Open(filepath.Join(tempDirectory(t), "store"), Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	who := NativeIdentity{Name: "After close", Email: "closed@example.invalid"}
	if err := store.ConfigureNativeIdentity(who, NativeIdentityOptions{Signer: replacement, RequireSignature: true}); err != nil {
		t.Fatal(err)
	}
	if err := replacement.Close(); err != nil {
		t.Fatal(err)
	}
	if err := replacement.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := replacement.PublicMetadata(); !errors.Is(err, ErrSigningKeyClosed) {
		t.Fatal("closed signer exposed metadata", err)
	}
	draft := json.RawMessage(`{"version":1,"parents":[],"message":"retained expanded signer","timestamp":7,"files":{}}`)
	if _, err := store.PrepareNativeRevision(draft); err != nil {
		t.Fatal("owner close invalidated Store", err)
	}
	before, _ := store.NativeIdentityMetadata()
	if err := store.ConfigureNativeIdentity(who, NativeIdentityOptions{Signer: replacement}); !errors.Is(err, ErrSigningKeyClosed) {
		t.Fatal("closed owner configured", err)
	}
	after, _ = store.NativeIdentityMetadata()
	if !bytes.Equal(before, after) {
		t.Fatal("closed owner destroyed previous identity")
	}
	if _, err := ProtectSigningKey([]byte{1}, password, PasswordKdfOptions{}); err == nil {
		t.Fatal("short seed accepted")
	}
	if _, err := ProtectSigningKey(nil, nil, PasswordKdfOptions{}); err == nil {
		t.Fatal("empty password accepted")
	}
	if _, err := ChangeSigningKeyPassword(sealed, password, nil, PasswordKdfOptions{}); err == nil {
		t.Fatal("empty replacement password accepted")
	}
	if _, err := OpenProtectedSigner(sealed[:103], password); err == nil {
		t.Fatal("short signing envelope accepted")
	}
	fresh, err := ProtectSigningKey(nil, password, PasswordKdfOptions{})
	if err != nil || bytes.Equal(fresh, sealed) {
		t.Fatal("fresh signer generation failed", err)
	}
}
