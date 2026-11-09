package syrnck

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

func sealedOwner(t *testing.T) *SealedKey {
	t.Helper()
	envelope, err := ProtectSealedKey(bytes.Repeat([]byte{0x71}, 32), []byte("client-only password"), PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	key, err := OpenProtectedSealedKey(envelope, []byte("client-only password"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := key.Close(); err != nil {
			t.Error(err)
		}
	})
	return key
}

func TestProtectedKeyEmptySlicesGenerateKeys(t *testing.T) {
	password := []byte("empty-slice generation")
	for name, protect := range map[string]func([]byte, []byte, PasswordKdfOptions) ([]byte, error){
		"storage": ProtectKey, "signing": ProtectSigningKey, "sealed": ProtectSealedKey,
	} {
		t.Run(name, func(t *testing.T) {
			backing := []byte{0x51}
			for _, empty := range [][]byte{make([]byte, 0), backing[:0]} {
				envelope, err := protect(empty, password, PasswordKdfOptions{})
				if err != nil || len(envelope) != ProtectedKeySize {
					t.Fatalf("empty slice did not generate a %s key: %v", name, err)
				}
				switch name {
				case "storage":
					owner, err := OpenProtectedKey(envelope, password)
					if err != nil {
						t.Fatal(err)
					}
					if err = owner.Close(); err != nil {
						t.Fatal(err)
					}
				case "signing":
					owner, err := OpenProtectedSigner(envelope, password)
					if err != nil {
						t.Fatal(err)
					}
					if err = owner.Close(); err != nil {
						t.Fatal(err)
					}
				case "sealed":
					owner, err := OpenProtectedSealedKey(envelope, password)
					if err != nil {
						t.Fatal(err)
					}
					if err = owner.Close(); err != nil {
						t.Fatal(err)
					}
				}
			}
			if backing[0] != 0x51 {
				t.Fatal("generation changed a caller's empty slice backing array")
			}
		})
	}
}

func TestSealedKeyOwnershipPurposeAndPasswordChange(t *testing.T) {
	raw := bytes.Repeat([]byte{0x71}, 32)
	password := []byte("clé client\x00exact bytes")
	beforeRaw, beforePassword := bytes.Clone(raw), bytes.Clone(password)
	envelope, err := ProtectSealedKey(raw, password, PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	if len(envelope) != ProtectedKeySize || !bytes.Equal(raw, beforeRaw) || !bytes.Equal(password, beforePassword) {
		t.Fatal("key protection changed borrowed buffers")
	}
	key, err := OpenProtectedSealedKey(envelope, password)
	if err != nil {
		t.Fatal(err)
	}
	defer key.Close()
	clear(raw)
	clear(password)
	metadata, err := key.PublicMetadata()
	if err != nil {
		t.Fatal(err)
	}
	var public map[string]any
	if err = json.Unmarshal(metadata, &public); err != nil {
		t.Fatal(err)
	}
	if len(public) != 3 || public["realm"] != "d9ec988a39dfbc52de95293fb84f3f03" {
		t.Fatalf("invalid or secret-bearing metadata: %s", metadata)
	}
	if _, err = OpenProtectedKey(envelope, beforePassword); err == nil {
		t.Fatal("sealed master opened as a storage key")
	}
	if _, err = OpenProtectedSigner(envelope, beforePassword); err == nil {
		t.Fatal("sealed master opened as a signer")
	}
	storage, err := ProtectKey(beforeRaw, beforePassword, PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	signer, err := ProtectSigningKey(beforeRaw, beforePassword, PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	for _, invalid := range [][]byte{storage, signer, bytes.Repeat([]byte{0}, ProtectedKeySize)} {
		if owner, err := OpenProtectedSealedKey(invalid, beforePassword); err == nil || owner != nil {
			t.Fatal("invalid purpose/format returned a sealed owner")
		}
	}
	if owner, err := OpenProtectedSealedKey(envelope, []byte("wrong")); err == nil || owner != nil {
		t.Fatal("wrong password returned a master owner")
	}
	changed, err := ChangeSealedKeyPassword(envelope, beforePassword, []byte("replacement"), PasswordKdfOptions{Passes: 4})
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(changed, envelope) {
		t.Fatal("password change reused its envelope")
	}
	newOwner, err := OpenProtectedSealedKey(changed, []byte("replacement"))
	if err != nil {
		t.Fatal(err)
	}
	defer newOwner.Close()
	after, err := newOwner.PublicMetadata()
	if err != nil || !bytes.Equal(after, metadata) {
		t.Fatalf("rewrap changed realm: %s %v", after, err)
	}
	if owner, err := OpenProtectedSealedKey(changed, beforePassword); err == nil || owner != nil {
		t.Fatal("new envelope accepted old password")
	}
	if _, err = ProtectSealedKey(beforeRaw[:31], beforePassword, PasswordKdfOptions{}); err == nil {
		t.Fatal("short master accepted")
	}
	if _, err = ProtectSealedKey(nil, nil, PasswordKdfOptions{}); err == nil {
		t.Fatal("empty password accepted")
	}
	generated, err := ProtectSealedKey(nil, beforePassword, PasswordKdfOptions{})
	if err != nil {
		t.Fatal(err)
	}
	generatedOwner, err := OpenProtectedSealedKey(generated, beforePassword)
	if err != nil {
		t.Fatal(err)
	}
	defer generatedOwner.Close()
	generatedPublic, err := generatedOwner.PublicMetadata()
	if err != nil || bytes.Equal(generatedPublic, metadata) {
		t.Fatal("generated master reused a fixed test key")
	}
	if err = key.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err = key.PublicMetadata(); !errors.Is(err, ErrSealedKeyClosed) {
		t.Fatal(err)
	}
	var absent *SealedKey
	if err = absent.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err = absent.PublicMetadata(); !errors.Is(err, ErrSealedKeyClosed) {
		t.Fatal(err)
	}
}

func TestSealedSnapshotsOpaqueCopyBlockReuseAndHistory(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			root := tempDirectory(t)
			var atRest []byte
			if encrypted {
				atRest = bytes.Repeat([]byte{0x31}, 32)
			}
			source := openTest(t, filepath.Join(root, "source-密钥"), atRest)
			wire := openTest(t, filepath.Join(root, "wire"), nil)
			if encrypted {
				atRest = bytes.Repeat([]byte{0x32}, 32)
			}
			relay := openTest(t, filepath.Join(root, "relay"), atRest)
			if encrypted {
				atRest = bytes.Repeat([]byte{0x33}, 32)
			}
			destination := openTest(t, filepath.Join(root, "destination"), atRest)
			key := sealedOwner(t)
			seed := bytes.Repeat([]byte{0x34}, 32)
			if err := source.ConfigureNativeIdentity(NativeIdentity{Name: "Client", Email: "client@example.invalid"}, NativeIdentityOptions{Seed: seed, RequireSignature: true}); err != nil {
				t.Fatal(err)
			}
			clear(seed)
			payload := append(append(bytes.Repeat([]byte{'a'}, 4096), bytes.Repeat([]byte{'b'}, 4096)...), bytes.Repeat([]byte{'c'}, 4096)...)
			chunking := Chunking{Fixed: true, Min: 4096, Average: 4096, Max: 4096}
			manifest, err := source.PrepareBytes(payload, chunking)
			if err != nil {
				t.Fatal(err)
			}
			first, err := source.PrepareNativeRevision(revisionDocument(t, manifest))
			if err != nil {
				t.Fatal(err)
			}
			if err = source.Pin("heads/private", first); err != nil {
				t.Fatal(err)
			}
			originalRefs, err := source.Refs()
			if err != nil {
				t.Fatal(err)
			}
			roots := map[string]ObjectKey{"heads/private": first}
			prepared, err := key.SealSnapshot(context.Background(), source, wire, "team", roots, "", SealedLimits{})
			if err != nil {
				t.Fatal(err)
			}
			if prepared.State != "prepared" || !reflect.DeepEqual(prepared.Roots, roots) || prepared.SealedObjects < 5 {
				t.Fatal(prepared)
			}
			if refs, err := source.Refs(); err != nil || !reflect.DeepEqual(refs, originalRefs) {
				t.Fatalf("sealing changed source references: %v %v", refs, err)
			}
			unchanged, err := key.SealSnapshot(context.Background(), source, wire, "team", roots, prepared.Snapshot, SealedLimits{})
			if err != nil || unchanged.State != "unchanged" || unchanged.Snapshot != prepared.Snapshot || unchanged.SealedObjects != 0 {
				t.Fatalf("unchanged snapshot rewrote envelopes: %+v %v", unchanged, err)
			}
			opaque := ObjectKey{Kind: "revision", ID: prepared.Snapshot}
			closure, err := wire.Closure(opaque)
			if err != nil {
				t.Fatal(err)
			}
			for _, object := range closure {
				data, err := wire.Get(object)
				if err != nil {
					t.Fatal(err)
				}
				for _, private := range [][]byte{[]byte("heads/private"), []byte(first.ID), []byte("Édit & Map.uasset"), payload} {
					if bytes.Contains(data, private) {
						t.Fatal("wire object exposed original data or identity")
					}
				}
			}
			stats, err := CopyMissing(context.Background(), wire, relay, opaque, "opaque/received")
			if err != nil || stats.Objects == 0 {
				t.Fatalf("opaque copy failed: %+v %v", stats, err)
			}
			verified, err := key.InspectSnapshot(context.Background(), relay, prepared.Snapshot, "team", SealedLimits{MaxIndexCacheBytes: 1024})
			if err != nil || !reflect.DeepEqual(verified.Roots, roots) {
				t.Fatalf("remote wire lost authenticated roots: %+v %v", verified, err)
			}
			opened, err := key.OpenSnapshot(context.Background(), relay, destination, prepared.Snapshot, "team", "sealed/checkpoints/team", SealedLimits{})
			if err != nil || opened.ImportedObjects != prepared.Objects {
				t.Fatalf("initial import failed: %+v %v", opened, err)
			}
			if refs, err := destination.Refs(); err != nil || refs["heads/private"].ID != "" {
				t.Fatalf("opening replaced original branches: %v %v", refs, err)
			}
			read, err := destination.ReadRange(manifest, 0, uint64(len(payload)))
			if err != nil || !bytes.Equal(read, payload) {
				t.Fatal("first sealed version changed", err)
			}
			updatedPayload := bytes.Clone(payload)
			copy(updatedPayload[4096:8192], bytes.Repeat([]byte{'d'}, 4096))
			updatedManifest, err := source.PrepareBytes(updatedPayload, chunking)
			if err != nil {
				t.Fatal(err)
			}
			second, err := source.PrepareNativeRevision(revisionDocument(t, updatedManifest, first.ID))
			if err != nil {
				t.Fatal(err)
			}
			updated, err := key.SealSnapshot(context.Background(), source, wire, "team", map[string]ObjectKey{"heads/private": second}, prepared.Snapshot, SealedLimits{})
			if err != nil || updated.SealedObjects != 3 || updated.ReusedObjects != prepared.Objects {
				t.Fatalf("one edited block re-encrypted unchanged objects: %+v %v", updated, err)
			}
			if _, err = CopyMissing(context.Background(), wire, relay, ObjectKey{Kind: "revision", ID: updated.Snapshot}, "opaque/received"); err != nil {
				t.Fatal(err)
			}
			if _, err = key.OpenSnapshot(context.Background(), relay, destination, updated.Snapshot, "team", "sealed/checkpoints/team", SealedLimits{}); err != nil {
				t.Fatal(err)
			}
			before, err := destination.Refs()
			if err != nil {
				t.Fatal(err)
			}
			if _, err = key.OpenSnapshot(context.Background(), relay, destination, prepared.Snapshot, "team", "sealed/checkpoints/team", SealedLimits{}); err == nil || !strings.Contains(err.Error(), "rolls back or forks") {
				t.Fatal("accepted sealed rollback", err)
			}
			if after, err := destination.Refs(); err != nil || !reflect.DeepEqual(before, after) {
				t.Fatal("rollback changed refs", err)
			}
			if _, err = key.OpenSnapshot(context.Background(), relay, destination, prepared.Snapshot, "team", "sealed/checkpoints/history", SealedLimits{}); err != nil {
				t.Fatal(err)
			}
			if _, err = key.InspectSnapshot(context.Background(), relay, prepared.Snapshot, "other", SealedLimits{}); err == nil {
				t.Fatal("accepted another channel")
			}
			if err = key.Close(); err != nil {
				t.Fatal(err)
			}
			for id, expected := range map[string][]byte{manifest: payload, updatedManifest: updatedPayload} {
				got, err := destination.ReadRange(id, 0, uint64(len(expected)))
				if err != nil || !bytes.Equal(got, expected) {
					t.Fatal("master close changed imported bytes", err)
				}
			}
			if _, err = destination.Verify(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestSealedCancellationLimitsAndSerialization(t *testing.T) {
	root := tempDirectory(t)
	source := openTest(t, filepath.Join(root, "source"), nil)
	wire := openTest(t, filepath.Join(root, "wire"), nil)
	target := openTest(t, filepath.Join(root, "target"), nil)
	key := sealedOwner(t)
	manifest, err := source.PrepareBytes(append(bytes.Repeat([]byte{'a'}, 1024), bytes.Repeat([]byte{'b'}, 1024)...), Chunking{Fixed: true, Min: 1024, Average: 1024, Max: 1024})
	if err != nil {
		t.Fatal(err)
	}
	roots := map[string]ObjectKey{"file": {Kind: "manifest", ID: manifest}}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err = key.SealSnapshot(ctx, source, wire, "main", roots, "", SealedLimits{}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	for _, limits := range []SealedLimits{{MaxObjects: 1}, {MaxWork: 1}, {MaxIndexCacheBytes: 512}, {MaxNewIndexBytes: 1}, {MaxHistory: 1000001}, {MaxObjects: math.MaxUint64}} {
		if _, err = key.SealSnapshot(context.Background(), source, wire, "main", roots, "", limits); err == nil {
			t.Fatalf("invalid/bounded sealed limits accepted: %+v", limits)
		}
	}
	if refs, err := wire.Refs(); err != nil || len(refs) != 0 {
		t.Fatal("failed preparation changed wire refs", err)
	}
	prepared, err := key.SealSnapshot(context.Background(), source, wire, "main", roots, "", SealedLimits{})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = key.InspectSnapshot(ctx, wire, prepared.Snapshot, "main", SealedLimits{}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if _, err = key.OpenSnapshot(ctx, wire, target, prepared.Snapshot, "main", "sealed/checkpoints/test", SealedLimits{}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if refs, err := target.Refs(); err != nil || len(refs) != 0 {
		t.Fatal("cancelled import changed target refs", err)
	}
	for _, mutex := range []*contextMutex{&key.mu, &wire.mu} {
		mutex.Lock()
		ctx, stop := context.WithTimeout(context.Background(), 20*time.Millisecond)
		_, err = key.InspectSnapshot(ctx, wire, prepared.Snapshot, "main", SealedLimits{})
		stop()
		mutex.Unlock()
		if !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("serialization wait ignored context: %v", err)
		}
	}
	if _, err = key.SealSnapshot(context.Background(), source, source, "main", roots, "", SealedLimits{}); err == nil {
		t.Fatal("same source/wire handle was accepted")
	}
	if _, err = key.InspectSnapshot(context.Background(), nil, prepared.Snapshot, "main", SealedLimits{}); !errors.Is(err, ErrClosed) {
		t.Fatal(err)
	}
	if err = target.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err = key.OpenSnapshot(context.Background(), wire, target, prepared.Snapshot, "main", "sealed/checkpoints/test", SealedLimits{}); !errors.Is(err, ErrClosed) {
		t.Fatal(err)
	}
	// Fresh goroutines, GC and stack growth exercise native output ownership and
	// tracked pointer conversions on Windows, plus cgo ownership on Unix.
	var wg sync.WaitGroup
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			var pad [8192]byte
			pad[0] = 1
			runtime.GC()
			for j := 0; j < 2; j++ {
				verified, err := key.InspectSnapshot(context.Background(), wire, prepared.Snapshot, "main", SealedLimits{})
				if err != nil || verified.Snapshot != prepared.Snapshot {
					t.Errorf("concurrent sealed inspect: %+v %v", verified, err)
				}
			}
			runtime.KeepAlive(pad)
		}()
	}
	wg.Wait()
	if err = key.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err = key.InspectSnapshot(context.Background(), wire, prepared.Snapshot, "main", SealedLimits{}); !errors.Is(err, ErrSealedKeyClosed) {
		t.Fatal(err)
	}
}

func TestSealedOwnerStoreOrderingAndCloseLifetime(t *testing.T) {
	root := tempDirectory(t)
	source := openTest(t, filepath.Join(root, "source"), nil)
	wire := openTest(t, filepath.Join(root, "wire"), nil)
	key, other := sealedOwner(t), sealedOwner(t)
	manifest, err := source.PrepareBytes([]byte("lifetime payload"), Chunking{})
	if err != nil {
		t.Fatal(err)
	}
	roots := map[string]ObjectKey{"file": {Kind: "manifest", ID: manifest}}
	prepared, err := key.SealSnapshot(context.Background(), source, wire, "main", roots, "", SealedLimits{})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	for i := 0; i < 5; i++ {
		results := make(chan error, 2)
		go func() { _, e := key.SealSnapshot(ctx, source, wire, "main", roots, "", SealedLimits{}); results <- e }()
		go func() {
			_, e := other.SealSnapshot(ctx, wire, source, "main", roots, "", SealedLimits{})
			if e == nil {
				e = errors.New("reverse source unexpectedly contained private originals")
			} else if !errors.Is(e, context.DeadlineExceeded) && !errors.Is(e, context.Canceled) {
				e = nil
			}
			results <- e
		}()
		for j := 0; j < 2; j++ {
			if e := <-results; e != nil {
				t.Fatal("opposite Store order deadlocked or failed", e)
			}
		}
	}
	entered, release := make(chan struct{}), make(chan struct{})
	work := make(chan error, 1)
	go func() {
		work <- key.withStores(ctx, []*Store{wire}, func(handle nativeSealedKeyHandle, cancellation nativeCancellationHandle) error {
			close(entered)
			<-release
			id, _ := text(prepared.Snapshot)
			channel, _ := text("main")
			_, e := nativeInspectSealed(wire.handle, handle, id, channel, nil, cancellation)
			return e
		})
	}()
	<-entered
	started := make(chan struct{}, 2)
	keyClosed, storeClosed := make(chan error, 1), make(chan error, 1)
	go func() { started <- struct{}{}; keyClosed <- key.Close() }()
	go func() { started <- struct{}{}; storeClosed <- wire.Close() }()
	<-started
	<-started
	select {
	case e := <-keyClosed:
		close(release)
		t.Fatal("master closed during its active native operation", e)
	case e := <-storeClosed:
		close(release)
		t.Fatal("wire closed during its active native operation", e)
	case <-time.After(20 * time.Millisecond):
	}
	close(release)
	if e := <-work; e != nil {
		t.Fatal("native use lost its owned master or Store", e)
	}
	if e := <-keyClosed; e != nil {
		t.Fatal(e)
	}
	if e := <-storeClosed; e != nil {
		t.Fatal(e)
	}
	if _, e := key.PublicMetadata(); !errors.Is(e, ErrSealedKeyClosed) {
		t.Fatal(e)
	}
}
