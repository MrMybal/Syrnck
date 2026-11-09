package syncthing

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"path/filepath"
	"strings"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func conflictCatalog(t *testing.T, store *syrnck.Store, localWins, same, localDelete, remoteDelete bool) (*Catalog, CatalogEntry, CatalogEntry) {
	t.Helper()
	options := catalogOptions()
	catalog := mustCatalog(t, store, options)
	local := preparedEntry(t, store, "Content/map.uasset", []byte("local asset bytes"))
	local.Info.ModifiedS = 10
	if localWins {
		local.Info.ModifiedS = 30
	}
	if localDelete {
		local.Info.Deleted = true
		local.Info.Blocks = nil
		local.Info.BlocksHash = nil
		local.Info.Size = 0
		local.Manifest = ""
	}
	if err := catalog.RecordLocal(local); err != nil {
		t.Fatal(err)
	}
	local = localEntry(t, catalog, local.Info.Name)
	data := []byte("remote asset bytes")
	if same {
		data = []byte("local asset bytes")
	}
	remote := preparedEntry(t, store, local.Info.Name, data)
	remote.Info.ModifiedS = 20
	remote.Info.Version = protocol.Vector{}.Update(options.Peers[0].Short())
	remote.Info.ModifiedBy = options.Peers[0].Short()
	remote.Info.Sequence = 1
	if remoteDelete {
		remote.Info.Deleted = true
		remote.Info.Blocks = nil
		remote.Info.BlocksHash = nil
		remote.Info.Size = 0
		remote.Manifest = ""
	}
	receiveCatalog(t, catalog, options, true, 0, 1, remote.Info)
	if !remoteDelete {
		if err := catalog.Attach(context.Background(), store, options.Peers[0], remote.Info.Name, remote.Manifest); err != nil {
			t.Fatal(err)
		}
	}
	mustCommitCatalog(t, store, catalog)
	return catalog, local, remote
}

func TestConflictValidationAndUnicodeNamespace(t *testing.T) {
	store := openStore(t, false)
	catalog, local, _ := conflictCatalog(t, store, false, false, false, false)
	for _, kind := range []string{"missing-data", "wrong-data", "vector-limit", "non-concurrent", "unknown-peer"} {
		t.Run(kind, func(t *testing.T) {
			draft := &Catalog{state: cloneCatalogState(catalog.state)}
			device := catalogOptions().Peers[0]
			switch kind {
			case "missing-data":
				draft.state.Peers[0].Files[0].Manifest = ""
			case "wrong-data":
				draft.state.Peers[0].Files[0].Manifest = local.Manifest
			case "vector-limit":
				draft.state.Files[0].Info.Version.Counters = nil
				draft.state.Peers[0].Files[0].Info.Version.Counters = nil
				for i := 1; i <= 256; i++ {
					draft.state.Files[0].Info.Version.Counters = append(draft.state.Files[0].Info.Version.Counters, protocol.Counter{ID: protocol.ShortID(i), Value: 1})
					draft.state.Peers[0].Files[0].Info.Version.Counters = append(draft.state.Peers[0].Files[0].Info.Version.Counters, protocol.Counter{ID: protocol.ShortID(i + 256), Value: 1})
				}
			case "non-concurrent":
				draft.state.Peers[0].Files[0].Info.Version = draft.state.Files[0].Info.Version
			case "unknown-peer":
				device = protocol.NewDeviceID([]byte("unpaired conflict"))
			}
			before, _ := json.Marshal(draft.state)
			if _, err := draft.ResolveConcurrent(context.Background(), store, device, local.Info.Name); err == nil {
				t.Fatal("invalid conflict accepted")
			}
			after, _ := json.Marshal(draft.state)
			if !bytes.Equal(before, after) {
				t.Fatal("invalid conflict mutated draft")
			}
		})
	}
	for _, occupied := range []string{"Content/STRASSE.bin", "Content/straße.bin/child", "CONTENT"} {
		if !conflictNameOccupied([]CatalogEntry{{Info: protocol.FileInfo{Name: occupied}}}, "content/straße.bin") {
			t.Fatal("case-fold or hierarchy collision missed", occupied)
		}
	}
	long := "Content/" + strings.Repeat("é", 180) + ".uasset"
	name := conflictCopyName(long, local.Info.Version, local.Info.ModifiedBy, "content", 0)
	if !nameValid(name) || filepath.Ext(name) != ".uasset" {
		t.Fatal("long Unicode name not portable", name)
	}
}

func TestConcurrentEqualContentsIgnoreNativeAndBEPChunkBoundaries(t *testing.T) {
	store := openStore(t, false)
	data := bytes.Join([][]byte{block(1), block(2), block(3), []byte("tail")}, nil)
	options := catalogOptions()
	catalog := mustCatalog(t, store, options)
	a, b := ingest(t, store, data, true), ingest(t, store, data, false)
	if a == b {
		t.Fatal("fixture needs different native chunk boundaries")
	}
	local, err := NewFile(context.Background(), store, a, options.Folder, "asset.uasset", protocol.MinBlockSize)
	if err != nil {
		t.Fatal(err)
	}
	remote, err := NewFile(context.Background(), store, b, options.Folder, "asset.uasset", 2*protocol.MinBlockSize)
	if err != nil {
		t.Fatal(err)
	}
	if err := catalog.RecordLocal(CatalogEntry{Info: local.Info(), Manifest: a}); err != nil {
		t.Fatal(err)
	}
	info := remote.Info()
	info.Version = protocol.Vector{}.Update(options.Peers[0].Short())
	info.ModifiedBy, info.ModifiedS, info.Sequence = options.Peers[0].Short(), 1, 1
	receiveCatalog(t, catalog, options, true, 0, 1, info)
	if err := catalog.Attach(context.Background(), store, options.Peers[0], info.Name, b); err != nil {
		t.Fatal(err)
	}
	result, err := catalog.ResolveConcurrent(context.Background(), store, options.Peers[0], info.Name)
	if err != nil || !result.Coalesced || result.Copy != "" || len(catalog.state.Files) != 1 {
		t.Fatal("identical bytes generated a conflict copy", result, err)
	}
	mustCommitCatalog(t, store, catalog)
}

func BenchmarkIdleCatalogAdopt(b *testing.B) {
	for _, count := range []int{32, 512, 4096} {
		b.Run(fmt.Sprint(count), func(b *testing.B) {
			options := catalogOptions()
			state, err := catalogConfiguration(options)
			if err != nil {
				b.Fatal(err)
			}
			for i := 0; i < count; i++ {
				entry := CatalogEntry{Info: protocol.FileInfo{Name: fmt.Sprintf("asset-%06d", i), Sequence: int64(i + 1), Version: protocol.Vector{}.Update(options.LocalDevice.Short())}, Manifest: strings.Repeat("a", 64)}
				state.Files = append(state.Files, entry)
				state.Peers[0].Files = append(state.Peers[0].Files, entry)
			}
			catalog := &Catalog{state: state}
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if err := catalog.Adopt(options.Peers[0], state.Files[i%count].Info.Name); err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}

func TestCatalogExplicitConflictCopiesAndCoalescing(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		for _, localWins := range []bool{false, true} {
			for _, kind := range []string{"different", "same", "local-delete", "peer-delete", "both-delete"} {
				t.Run(fmt.Sprintf("%v/%v/%s", encrypted, localWins, kind), func(t *testing.T) {
					var key []byte
					if encrypted {
						key = bytes.Repeat([]byte{64}, 32)
					}
					store, err := syrnck.Open(filepath.Join(tempDirectory(t), "store"), syrnck.Options{Create: true, Key: key})
					if err != nil {
						t.Fatal(err)
					}
					defer store.Close()
					catalog, local, remote := conflictCatalog(t, store, localWins, kind == "same", kind == "local-delete" || kind == "both-delete", kind == "peer-delete" || kind == "both-delete")
					before, _ := json.Marshal(catalog.state)
					if err := catalog.Adopt(catalogOptions().Peers[0], local.Info.Name); !errors.Is(err, ErrCatalogConflict) {
						t.Fatal("ordinary adopt did not refuse conflict", err)
					}
					result, err := catalog.ResolveConcurrent(context.Background(), store, catalogOptions().Peers[0], local.Info.Name)
					if err != nil {
						t.Fatal(err)
					}
					want, loser := remote, local
					if localWins {
						want, loser = local, remote
					}
					current := localEntry(t, catalog, local.Info.Name)
					if current.Manifest != want.Manifest || current.Info.Deleted != want.Info.Deleted ||
						current.Info.Version.Compare(local.Info.Version) != protocol.Greater || current.Info.Version.Compare(remote.Info.Version) != protocol.Greater {
						t.Fatal("conflict winner or merged vector differs", result, current.Info.Version)
					}
					copies := kind != "same" && kind != "both-delete" && !loser.Info.Deleted
					if copies != (result.Copy != "") {
						t.Fatal("losing contents not represented exactly once", result)
					}
					if copies {
						copy := localEntry(t, catalog, result.Copy)
						if copy.Manifest != loser.Manifest || copy.Info.Version.Compare(loser.Info.Version) != protocol.Equal || filepath.Ext(copy.Info.Name) != ".uasset" {
							t.Fatal("copy lost origin vector, extension or contents", copy)
						}
					}
					if result.Coalesced != (kind == "same" || kind == "both-delete") {
						t.Fatal("coalescing report differs", result)
					}
					if after, _ := json.Marshal(catalog.state); bytes.Equal(before, after) {
						t.Fatal("resolved draft did not advance")
					}
					mustCommitCatalog(t, store, catalog)
					reopened := mustCatalog(t, store, catalogOptions())
					if localEntry(t, reopened, local.Info.Name).Info.Version.Compare(current.Info.Version) != protocol.Equal {
						t.Fatal("resolved state did not persist")
					}
					if _, err := store.Verify(); err != nil {
						t.Fatal(err)
					}
				})
			}
		}
	}
}

func TestCatalogConflictFailureIsAtomicAndCopyNamesAreStable(t *testing.T) {
	store := openStore(t, false)
	catalog, local, _ := conflictCatalog(t, store, false, false, false, false)
	before, _ := json.Marshal(catalog.state)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := catalog.ResolveConcurrent(ctx, store, catalogOptions().Peers[0], local.Info.Name); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if after, _ := json.Marshal(catalog.state); !bytes.Equal(before, after) {
		t.Fatal("cancelled conflict changed draft")
	}
	state := cloneCatalogState(catalog.state)
	first, err := catalog.ResolveConcurrent(context.Background(), store, catalogOptions().Peers[0], local.Info.Name)
	if err != nil {
		t.Fatal(err)
	}
	clone := &Catalog{state: state}
	second, err := clone.ResolveConcurrent(context.Background(), store, catalogOptions().Peers[0], local.Info.Name)
	if err != nil || first.Copy != second.Copy {
		t.Fatal("same conflict produced a different copy", first, second, err)
	}
	clone = &Catalog{state: cloneCatalogState(state)}
	clone.state.Sequence = math.MaxInt64
	if _, err := clone.ResolveConcurrent(context.Background(), store, catalogOptions().Peers[0], local.Info.Name); err == nil {
		t.Fatal("sequence overflow accepted")
	}
	if clone.state.Sequence != math.MaxInt64 {
		t.Fatal("failed conflict changed sequence")
	}
	clone = &Catalog{state: cloneCatalogState(state)}
	occupied := local
	occupied.Info.Name = first.Copy
	occupied.Info.Sequence = clone.state.Sequence + 1
	clone.state.Sequence++
	clone.state.Files = setCatalogEntry(clone.state.Files, occupied)
	third, err := clone.ResolveConcurrent(context.Background(), store, catalogOptions().Peers[0], local.Info.Name)
	if err != nil || third.Copy == first.Copy || localEntry(t, clone, first.Copy).Manifest != occupied.Manifest {
		t.Fatal("occupied copy path was replaced", third, err)
	}
}
