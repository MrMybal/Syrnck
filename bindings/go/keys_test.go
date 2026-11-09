package syrnck

import (
	"bytes"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func TestProtectedKeyOwnershipRewrapAndRetainedStore(t *testing.T) {
	root := tempDirectory(t)
	raw := bytes.Repeat([]byte{43}, 32)
	retainedRaw := bytes.Clone(raw)
	password := []byte("clé locale\x00exact bytes")
	retainedPassword := bytes.Clone(password)
	envelope, err := ProtectKey(raw, password, PasswordKdfOptions{MemoryKiB: 65 * 1024, Passes: 4})
	if err != nil {
		t.Fatal(err)
	}
	original := bytes.Clone(envelope)
	if len(envelope) != ProtectedKeySize || !bytes.Equal(raw, retainedRaw) || !bytes.Equal(password, retainedPassword) {
		t.Fatal("protection changed borrowed inputs")
	}
	key, err := OpenProtectedKey(envelope, password)
	if err != nil {
		t.Fatal(err)
	}
	defer key.Close()
	clear(raw)
	clear(password)
	storePath := filepath.Join(root, "données")
	store, err := key.Open(storePath, true)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	payload := bytes.Repeat([]byte("native-owned protected bytes\x00"), 4096)
	id, err := store.PrepareBytes(payload, Chunking{})
	if err != nil {
		t.Fatal(err)
	}
	if err = store.Pin("retained/key-test", ObjectKey{Kind: "manifest", ID: id}); err != nil {
		t.Fatal(err)
	}
	if err = key.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err = key.Open(filepath.Join(root, "closed-key"), true); !errors.Is(err, ErrSecretKeyClosed) {
		t.Fatalf("closed owner: %v", err)
	}
	if _, err = os.Stat(filepath.Join(root, "closed-key")); !os.IsNotExist(err) {
		t.Fatal("closed owner created a Store")
	}
	data, err := store.ReadRange(id, 0, uint64(len(payload)))
	if err != nil || !bytes.Equal(data, payload) {
		t.Fatalf("closing owner invalidated Store: %v", err)
	}
	if err = store.Close(); err != nil {
		t.Fatal(err)
	}
	nextPassword := []byte("autre mot de passe")
	rewrapped, err := ChangeKeyPassword(envelope, retainedPassword, nextPassword, PasswordKdfOptions{Passes: 5})
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(rewrapped, envelope) || !bytes.Equal(envelope, original) || binary.LittleEndian.Uint32(rewrapped[8:12]) != 65*1024 || binary.LittleEndian.Uint32(rewrapped[12:16]) != 5 {
		t.Fatal("rewrap changed input, reused envelope or lost unspecified costs")
	}
	if _, err = OpenProtectedKey(rewrapped, retainedPassword); err == nil {
		t.Fatal("old password accepted new envelope")
	}
	reopened, err := OpenProtected(storePath, rewrapped, nextPassword, false)
	if err != nil {
		t.Fatal(err)
	}
	data, err = reopened.ReadRange(id, 0, uint64(len(payload)))
	if err != nil || !bytes.Equal(data, payload) {
		t.Fatalf("rewrap lost retained data: %v", err)
	}
	if err = reopened.Close(); err != nil {
		t.Fatal(err)
	}
	rawStore, err := Open(storePath, Options{Key: retainedRaw})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = rawStore.Verify(); err != nil {
		t.Fatal(err)
	}
	rawStore.Close()
	old, err := OpenProtectedKey(envelope, retainedPassword)
	if err != nil {
		t.Fatal("old envelopes must not claim revocation", err)
	}
	old.Close()
}

func TestProtectedKeyGenerationRefusalsAndConcurrentClose(t *testing.T) {
	password := []byte("generated secret")
	envelope, err := ProtectKey(nil, password, PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	root := tempDirectory(t)
	if _, err = OpenProtected(filepath.Join(root, "wrong"), envelope, []byte("wrong password"), true); err == nil {
		t.Fatal("wrong password accepted")
	}
	if _, err = os.Stat(filepath.Join(root, "wrong")); !os.IsNotExist(err) {
		t.Fatal("authentication failure created a directory")
	}
	damaged := bytes.Clone(envelope)
	damaged[88] ^= 1
	if _, err = OpenProtectedKey(damaged, password); err == nil {
		t.Fatal("damaged envelope accepted")
	}
	for _, costs := range []PasswordKdfOptions{{MemoryKiB: 1}, {MemoryKiB: 65537}, {MemoryKiB: ^uint32(0)}, {Passes: 2}, {Passes: 11}} {
		if _, err = ProtectKey(nil, password, costs); err == nil {
			t.Fatal("invalid resource cost accepted")
		}
	}
	if _, err = ProtectKey([]byte{1}, password, PasswordKdfOptions{}); err == nil {
		t.Fatal("short key accepted")
	}
	if _, err = ProtectKey(nil, nil, PasswordKdfOptions{}); err == nil {
		t.Fatal("empty password accepted")
	}
	if _, err = OpenProtectedKey(nil, password); err == nil {
		t.Fatal("empty envelope accepted")
	}
	key, err := OpenProtectedKey(envelope, password)
	if err != nil {
		t.Fatal(err)
	}
	defer key.Close()
	var group sync.WaitGroup
	for index := 0; index < 8; index++ {
		group.Add(1)
		go func(index int) {
			defer group.Done()
			store, openErr := key.Open(filepath.Join(root, string(rune('a'+index))), true)
			if errors.Is(openErr, ErrSecretKeyClosed) {
				return
			}
			if openErr != nil {
				t.Error(openErr)
				return
			}
			if closeErr := store.Close(); closeErr != nil {
				t.Error(closeErr)
			}
		}(index)
	}
	group.Add(1)
	go func() {
		defer group.Done()
		if err := key.Close(); err != nil {
			t.Error(err)
		}
	}()
	group.Wait()
}
