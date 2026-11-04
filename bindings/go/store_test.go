package syrnck

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func openTest(t *testing.T, path string, key []byte) *Store {
	t.Helper()
	s, err := Open(path, Options{Create: true, Key: key})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := s.Close(); err != nil {
			t.Error(err)
		}
	})
	return s
}

func writeTest(t *testing.T, path string, content []byte) {
	t.Helper()
	if err := os.WriteFile(path, content, 0600); err != nil {
		t.Fatal(err)
	}
}

func TestFileRangeAndComparison(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			dir := tempDirectory(t)
			var key []byte
			if encrypted {
				key = bytes.Repeat([]byte{91}, 32)
			}
			s := openTest(t, filepath.Join(dir, "store"), key)
			original := append(append(bytes.Repeat([]byte{'a'}, 1024), bytes.Repeat([]byte{'b'}, 1024)...), bytes.Repeat([]byte{'a'}, 1024)...)
			changed := append(append(append(bytes.Repeat([]byte{'c'}, 1024), original[:1024]...), bytes.Repeat([]byte{'d'}, 2048)...), original[1024:2048]...)
			path := filepath.Join(dir, "input")
			config := Chunking{Fixed: true, Min: 1024, Average: 1024, Max: 1024}
			writeTest(t, path, original)
			a, err := s.Ingest(path, config)
			if err != nil {
				t.Fatal(err)
			}
			writeTest(t, path, changed)
			b, err := s.Ingest(path, config)
			if err != nil {
				t.Fatal(err)
			}
			part, err := s.ReadRange(b, 1000, 99)
			if err != nil || !bytes.Equal(part, changed[1000:1099]) {
				t.Fatalf("range differs: %v", err)
			}
			part, err = s.ReadRange(b, uint64(len(changed)), 0)
			if err != nil || len(part) != 0 {
				t.Fatalf("EOF range differs: %v", err)
			}
			for _, request := range [][2]uint64{{^uint64(0), 2}, {0, MaxObjectSize + 1}, {uint64(len(changed)), 1}} {
				if _, err = s.ReadRange(b, request[0], request[1]); err == nil {
					t.Fatal("invalid range accepted")
				}
			}
			out := filepath.Join(dir, "slice-échantillon")
			if err = s.RestoreRange(b, out, 1000, 99); err != nil {
				t.Fatal(err)
			}
			actual, err := os.ReadFile(out)
			if err != nil || !bytes.Equal(actual, changed[1000:1099]) {
				t.Fatal("exported range differs", err)
			}
			if err = s.RestoreRange(b, out, 0, 1); err == nil {
				t.Fatal("existing output overwritten")
			}
			plan, err := s.CompareFiles(a, b)
			if err != nil {
				t.Fatal(err)
			}
			if plan.ReusedBytes != 2048 || plan.NewBytes != 3072 || plan.UniqueNewBytes != 2048 || len(plan.NeededChunks) != 2 {
				t.Fatalf("plan differs: %+v", plan)
			}
			var reconstructed []byte
			for _, block := range plan.Blocks {
				if block.SourceOffset != nil {
					start := *block.SourceOffset
					reconstructed = append(reconstructed, original[start:start+block.Size]...)
				} else {
					data, e := s.Get(ObjectKey{Kind: "chunk", ID: block.ID})
					if e != nil {
						t.Fatal(e)
					}
					reconstructed = append(reconstructed, data...)
				}
			}
			if !bytes.Equal(reconstructed, changed) {
				t.Fatal("plan reconstruction differs")
			}
			writeTest(t, path, nil)
			empty, err := s.Ingest(path, config)
			if err != nil {
				t.Fatal(err)
			}
			if part, err = s.ReadRange(empty, 0, 0); err != nil || len(part) != 0 {
				t.Fatal("empty range differs", err)
			}
			if err = s.Close(); err != nil {
				t.Fatal(err)
			}
			if _, err = s.ReadRange(b, 0, 1); !errors.Is(err, ErrClosed) {
				t.Fatal(err)
			}
			if _, err = s.CompareFiles(a, b); !errors.Is(err, ErrClosed) {
				t.Fatal(err)
			}
		})
	}
}

func TestDifferentialTransfer(t *testing.T) {
	dir := tempDirectory(t)
	// The stores use different at-rest encryption keys. Object IDs stay stable.
	source := openTest(t, filepath.Join(dir, "source"), bytes.Repeat([]byte{17}, 32))
	dest := openTest(t, filepath.Join(dir, "destination"), bytes.Repeat([]byte{39}, 32))
	data := make([]byte, 8*1024*1024)
	rand.New(rand.NewSource(42)).Read(data)
	path := filepath.Join(dir, "Carte-échantillon.uasset")
	writeTest(t, path, data)
	id, err := source.Ingest(path, Chunking{})
	if err != nil {
		t.Fatal(err)
	}
	root := ObjectKey{"manifest", id}
	initial, err := CopyMissing(context.Background(), source, dest, root, "sync/asset-v1")
	if err != nil {
		t.Fatal(err)
	}
	if initial.Bytes < uint64(len(data)) {
		t.Fatal(initial)
	}
	// Insert bytes: CDC should realign and reuse most blocks.
	changed := append(append(append([]byte{}, data[:1024*1024]...), []byte("inserted bytes inside an asset")...), data[1024*1024:]...)
	writeTest(t, path, changed)
	newID, err := source.Ingest(path, Chunking{})
	if err != nil {
		t.Fatal(err)
	}
	root.ID = newID
	stats, err := CopyMissing(context.Background(), source, dest, root, "sync/asset-v2")
	if err != nil {
		t.Fatal(err)
	}
	if stats.ReusedObjects == 0 || stats.Bytes >= uint64(len(changed))*3/4 {
		t.Fatalf("insufficient reuse: %+v", stats)
	}
	output := filepath.Join(dir, "reconstructed.uasset")
	if err = dest.Restore(newID, output); err != nil {
		t.Fatal(err)
	}
	actual, err := os.ReadFile(output)
	if err != nil || !bytes.Equal(actual, changed) {
		t.Fatalf("reconstruction differs: %v", err)
	}
	if err = dest.Restore(newID, output); err == nil {
		t.Fatal("overwrote existing output")
	}
	retry, err := CopyMissing(context.Background(), source, dest, root, "sync/asset-v2")
	if err != nil || retry.Bytes != 0 || retry.Objects != 0 {
		t.Fatalf("retry %+v: %v", retry, err)
	}
	// The earlier pinned version is still reconstructible after replacing the source.
	oldOutput := filepath.Join(dir, "version1.uasset")
	if err = dest.Restore(id, oldOutput); err != nil {
		t.Fatal(err)
	}
	actual, _ = os.ReadFile(oldOutput)
	if !bytes.Equal(actual, data) {
		t.Fatal("old version lost")
	}
	if _, err = dest.Verify(); err != nil {
		t.Fatal(err)
	}
	t.Logf("CDC insertion: %d bytes transferred for %d-byte file, %d objects reused", stats.Bytes, len(changed), stats.ReusedObjects)
}

func TestImportResumeAndValidation(t *testing.T) {
	dir := tempDirectory(t)
	source := openTest(t, filepath.Join(dir, "source"), nil)
	destPath := filepath.Join(dir, "destination")
	dest := openTest(t, destPath, nil)
	path := filepath.Join(dir, "source.bin")
	writeTest(t, path, bytes.Repeat([]byte("ABC\x00EF"), 65536))
	id, err := source.Ingest(path, Chunking{true, 65536, 65536, 65536})
	if err != nil {
		t.Fatal(err)
	}
	root := ObjectKey{"manifest", id}
	data, err := source.Get(root)
	if err != nil {
		t.Fatal(err)
	}
	var manifest struct {
		Chunking struct {
			Algorithm string `json:"algorithm"`
		} `json:"chunking"`
	}
	if err = json.Unmarshal(data, &manifest); err != nil || manifest.Chunking.Algorithm != "fixed" {
		t.Fatalf("fixed settings: %v", err)
	}
	created, err := dest.Import(root, data)
	if err != nil || !created {
		t.Fatalf("import: %v", err)
	}
	if err = dest.Pin("sync/incomplete", root); err == nil {
		t.Fatal("published incomplete file")
	}
	tampered := append([]byte{}, data...)
	tampered[0] ^= 1
	if _, err = dest.Import(root, tampered); err == nil {
		t.Fatal("accepted incorrect hash")
	}
	// Simulate process restart with a partial transfer already on disk.
	if err = dest.Close(); err != nil {
		t.Fatal(err)
	}
	dest, err = Open(destPath, Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer dest.Close()
	stats, err := CopyMissing(context.Background(), source, dest, root, "sync/complete")
	if err != nil || stats.ReusedObjects == 0 {
		t.Fatalf("resume %+v: %v", stats, err)
	}
	if _, err = dest.Verify(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err = CopyMissing(ctx, source, dest, root, "sync/cancelled"); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if err = dest.Unpin("sync/complete"); err != nil {
		t.Fatal(err)
	}
}

func TestConcurrencyErrorsAndClose(t *testing.T) {
	dir := tempDirectory(t)
	key := bytes.Repeat([]byte{81}, 32)
	path := filepath.Join(dir, "store")
	s := openTest(t, path, key)
	if _, err := Open(path, Options{Key: key}); err == nil {
		t.Fatal("exclusive lock not enforced")
	}
	var wg sync.WaitGroup
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 8; j++ {
				if _, err := s.Get(ObjectKey{"chunk", "bad"}); err == nil || !strings.Contains(err.Error(), "64 lowercase") {
					t.Errorf("lost native error: %v", err)
				}
				if ok, err := s.Has(ObjectKey{"chunk", strings.Repeat("0", 64)}); ok || err != nil {
					t.Errorf("missing object: %v", err)
				}
			}
		}()
	}
	wg.Wait()
	if _, err := s.Ingest("bad\x00path", Chunking{}); err == nil {
		t.Fatal("accepted NUL")
	}
	if _, err := s.Ingest("bad\xffpath", Chunking{}); err == nil {
		t.Fatal("accepted invalid UTF-8")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Verify(); !errors.Is(err, ErrClosed) {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(path, Options{Key: bytes.Repeat([]byte{82}, 32)}); err == nil {
		t.Fatal("accepted wrong encryption key")
	}
	// A failed constructor must also release its exclusive native lock.
	reopened, err := Open(path, Options{Key: key})
	if err != nil {
		t.Fatalf("native handle was not released: %v", err)
	}
	if err := reopened.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestEmptyFileAndSameStore(t *testing.T) {
	dir := tempDirectory(t)
	s := openTest(t, filepath.Join(dir, "store"), nil)
	path := filepath.Join(dir, "empty")
	writeTest(t, path, nil)
	id, err := s.Ingest(path, Chunking{})
	if err != nil {
		t.Fatal(err)
	}
	stats, err := CopyMissing(context.Background(), s, s, ObjectKey{"manifest", id}, "empty")
	if err != nil || stats.Bytes != 0 || stats.ReusedObjects != 1 {
		t.Fatalf("same store: %+v %v", stats, err)
	}
	if err = s.Restore(id, filepath.Join(dir, "empty-copy")); err != nil {
		t.Fatal(err)
	}
}
