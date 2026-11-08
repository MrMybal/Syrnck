package syrnck

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"testing"
)

func revisionDocument(t *testing.T, manifest string, parents ...string) []byte {
	t.Helper()
	if parents == nil {
		parents = []string{}
	}
	data, err := json.Marshal(map[string]any{"version": 1, "parents": parents, "message": "État <&>", "timestamp": 1,
		"files": map[string]any{"data/Édit & Map.uasset": map[string]any{"manifest": manifest, "executable": false}}})
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestPreparedCatalogAtomicRetentionAndReopen(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			root := filepath.Join(tempDirectory(t), "store")
			var key []byte
			if encrypted {
				key = bytes.Repeat([]byte{58}, 32)
			}
			store := openTest(t, root, key)
			payload := bytes.Repeat([]byte("\x00first asset bytes\n"), 10000)
			chunking := Chunking{Fixed: true, Min: 65536, Average: 65536, Max: 65536}
			manifest, err := store.PrepareBytes(payload, chunking)
			if err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(tempDirectory(t), "input")
			writeTest(t, path, payload)
			streamed, err := store.PrepareFile(path, chunking)
			if err != nil || streamed != manifest {
				t.Fatalf("stream/buffer identity differs: %v", err)
			}
			refs, err := store.Refs()
			if err != nil || len(refs) != 0 {
				t.Fatalf("preparation published refs: %v %v", refs, err)
			}
			first, err := store.Put("revision", revisionDocument(t, manifest))
			if err != nil {
				t.Fatal(err)
			}
			data, err := store.Get(first)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.HasPrefix(data, []byte(`{"version":1,"parents":[],"message":`)) || bytes.Contains(data, []byte(`\u003c`)) {
				t.Fatal("Go JSON was not canonically encoded by the native engine")
			}
			manifestKey := ObjectKey{Kind: "manifest", ID: manifest}
			ok, err := store.CompareRefs([]RefUpdate{{Name: "ffi/catalog/main", Value: &first}, {Name: "ffi/catalog/snapshot", Value: &first},
				{Name: "ffi/catalog/extra", Value: &manifestKey}})
			if err != nil || !ok {
				t.Fatalf("initial atomic publication: %v %v", ok, err)
			}
			before, _ := store.Refs()
			secondPayload := []byte("second asset version")
			secondManifest, err := store.PrepareBytes(secondPayload, Chunking{})
			if err != nil {
				t.Fatal(err)
			}
			second, err := store.Put("revision", revisionDocument(t, secondManifest, first.ID))
			if err != nil {
				t.Fatal(err)
			}
			ok, err = store.CompareRefs([]RefUpdate{{Name: "ffi/catalog/main", Expected: &first, Value: &second}, {Name: "ffi/catalog/snapshot", Value: &second}})
			after, _ := store.Refs()
			if err != nil || ok || !reflect.DeepEqual(before, after) {
				t.Fatalf("stale group changed another ref: %v %v", ok, err)
			}
			ok, err = store.CompareRefs([]RefUpdate{{Name: "ffi/catalog/main", Expected: &first, Value: &second},
				{Name: "ffi/catalog/snapshot", Expected: &first, Value: &second}, {Name: "ffi/catalog/extra", Expected: &manifestKey}})
			if err != nil || !ok {
				t.Fatalf("replace/remove atomic group: %v %v", ok, err)
			}
			references, _ := store.Refs()
			if len(references) != 2 || references["ffi/catalog/main"] != second || references["ffi/catalog/snapshot"] != second {
				t.Fatal(references)
			}
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}
			if cli := os.Getenv("SYRNCK_TEST_CLI"); cli != "" {
				command := exec.Command(cli, "--store", root, "gc", "--delete")
				for _, entry := range os.Environ() {
					if !strings.HasPrefix(strings.ToUpper(entry), "SYRNCK_KEY=") {
						command.Env = append(command.Env, entry)
					}
				}
				if encrypted {
					command.Env = append(command.Env, "SYRNCK_KEY="+hex.EncodeToString(key))
				}
				if output, err := command.CombinedOutput(); err != nil {
					t.Fatalf("catalog GC: %v %s", err, output)
				}
			}
			reopened, err := Open(root, Options{Key: key})
			if err != nil {
				t.Fatal(err)
			}
			defer reopened.Close()
			for id, expected := range map[string][]byte{manifest: payload, secondManifest: secondPayload} {
				actual, err := reopened.ReadRange(id, 0, uint64(len(expected)))
				if err != nil || !bytes.Equal(actual, expected) {
					t.Fatalf("retained complete version lost: %v", err)
				}
			}
			current, err := reopened.Refs()
			if err != nil || !reflect.DeepEqual(current, references) {
				t.Fatalf("catalog reference restore: %v", err)
			}
			if _, err := reopened.Verify(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestCatalogCreationRejectsAmbiguousJSONAndBadClosures(t *testing.T) {
	root := filepath.Join(tempDirectory(t), "store")
	store := openTest(t, root, nil)
	manifest, err := store.PrepareBytes([]byte("content"), Chunking{})
	if err != nil {
		t.Fatal(err)
	}
	good, err := store.Put("revision", revisionDocument(t, manifest))
	if err != nil {
		t.Fatal(err)
	}
	if ok, err := store.CompareRefs([]RefUpdate{{Name: "ffi/current", Value: &good}}); err != nil || !ok {
		t.Fatal(err)
	}
	before, _ := store.Refs()
	valid := string(revisionDocument(t, manifest))
	for _, input := range []string{strings.Replace(valid, `"version":1`, `"version":1,"version":1`, 1),
		strings.Replace(valid, `"version":1`, `"version":1,"ver\u0073ion":1`, 1), `{`, strings.Repeat("[", 129) + strings.Repeat("]", 129)} {
		if _, err := store.Put("revision", []byte(input)); err == nil {
			t.Fatalf("invalid metadata accepted: %.80s", input)
		}
	}
	metadata, err := store.Get(ObjectKey{Kind: "manifest", ID: manifest})
	if err != nil {
		t.Fatal(err)
	}
	var malformed map[string]any
	if err := json.Unmarshal(metadata, &malformed); err != nil {
		t.Fatal(err)
	}
	malformed["file_hash"] = strings.Repeat("0", 64)
	encoded, _ := json.Marshal(malformed)
	bad, err := store.Put("manifest", encoded)
	if err != nil {
		t.Fatal(err)
	}
	if ok, err := store.CompareRefs([]RefUpdate{{Name: "ffi/current", Expected: &good, Value: &bad}, {Name: "ffi/other", Value: &good}}); err == nil || ok {
		t.Fatal("bad full-file hash published")
	}
	if ok, err := store.CompareRefs([]RefUpdate{{Name: "ffi/current", Expected: &good, Value: &good}, {Name: "ffi/current", Expected: &good}}); err == nil || ok {
		t.Fatal("duplicate reference name accepted")
	}
	after, _ := store.Refs()
	if !reflect.DeepEqual(before, after) {
		t.Fatal("failed catalog mutation changed references")
	}
	refsBytes, err := os.ReadFile(filepath.Join(root, "refs.json"))
	if err != nil {
		t.Fatal(err)
	}
	if ok, err := store.CompareRefs(nil); err != nil || !ok {
		t.Fatalf("empty transaction: %v %v", ok, err)
	}
	unchanged, err := os.ReadFile(filepath.Join(root, "refs.json"))
	if err != nil || !bytes.Equal(unchanged, refsBytes) {
		t.Fatal("empty transaction rewrote refs")
	}
}

func TestConcurrentCatalogCASAndCancellation(t *testing.T) {
	store := openTest(t, filepath.Join(tempDirectory(t), "store"), nil)
	left, err := store.Put("chunk", []byte("left"))
	if err != nil {
		t.Fatal(err)
	}
	right, err := store.Put("chunk", []byte("right"))
	if err != nil {
		t.Fatal(err)
	}
	var wait sync.WaitGroup
	results := make(chan bool, 2)
	failures := make(chan error, 2)
	for _, candidate := range []ObjectKey{left, right} {
		wait.Add(1)
		go func(value ObjectKey) {
			defer wait.Done()
			ok, err := store.CompareRefs([]RefUpdate{{Name: "ffi/winner", Value: &value}})
			results <- ok
			failures <- err
		}(candidate)
	}
	wait.Wait()
	close(results)
	close(failures)
	count := 0
	for ok := range results {
		if ok {
			count++
		}
	}
	for err := range failures {
		if err != nil {
			t.Fatal(err)
		}
	}
	if count != 1 {
		t.Fatal("CAS did not select exactly one winner")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := store.PrepareBytesContext(ctx, []byte("cancelled"), Chunking{}); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if ok, err := store.CompareRefsContext(ctx, []RefUpdate{{Name: "ffi/cancelled", Value: &left}}); ok || !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if _, err := store.RefsContext(ctx); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if _, err := store.PutContext(ctx, "chunk", []byte("cancelled")); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	refs, _ := store.Refs()
	if len(refs) != 1 {
		t.Fatal("cancelled SDK operation published a root")
	}
}
