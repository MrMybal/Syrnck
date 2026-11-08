package syncthing

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
	"golang.org/x/text/unicode/norm"
	syrnck "syrnck.local/bindings/go"
)

func catalogOptions() CatalogOptions {
	return CatalogOptions{Folder: "project", LocalDevice: protocol.NewDeviceID([]byte("catalog local certificate")),
		Peers: []protocol.DeviceID{protocol.NewDeviceID([]byte("catalog paired certificate"))}}
}

func mustCatalog(t *testing.T, store *syrnck.Store, options CatalogOptions) *Catalog {
	t.Helper()
	catalog, err := OpenCatalog(context.Background(), store, options)
	if err != nil {
		t.Fatal(err)
	}
	return catalog
}

func mustCommitCatalog(t *testing.T, store *syrnck.Store, catalog *Catalog) {
	t.Helper()
	if err := catalog.Commit(context.Background(), store); err != nil {
		t.Fatal(err)
	}
}

func preparedEntry(t *testing.T, store *syrnck.Store, name string, data []byte) CatalogEntry {
	t.Helper()
	manifest, err := store.PrepareBytes(data, syrnck.Chunking{})
	if err != nil {
		t.Fatal(err)
	}
	view, err := NewFile(context.Background(), store, manifest, "project", name, protocol.MinBlockSize)
	if err != nil {
		t.Fatal(err)
	}
	return CatalogEntry{Info: view.Info(), Manifest: manifest}
}

func localEntry(t *testing.T, catalog *Catalog, name string) CatalogEntry {
	t.Helper()
	_, _, files := catalog.LocalIndex()
	for _, entry := range files {
		if entry.Info.Name == name {
			return entry
		}
	}
	t.Fatalf("local entry missing: %q", name)
	return CatalogEntry{}
}

func receiveCatalog(t *testing.T, catalog *Catalog, options CatalogOptions, reset bool, prev, last int64, files ...protocol.FileInfo) {
	t.Helper()
	if err := catalog.ReceiveIndex(CatalogIndex{Device: options.Peers[0], IndexID: 23, Reset: reset, PrevSequence: prev, LastSequence: last, Files: files}); err != nil {
		t.Fatal(err)
	}
}

func TestCatalogOfflineHistoryConflictsAndGC(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			ctx := context.Background()
			root := filepath.Join(tempDirectory(t), "store")
			var key []byte
			if encrypted {
				key = bytes.Repeat([]byte{92}, 32)
			}
			store, err := syrnck.Open(root, syrnck.Options{Create: true, Key: key})
			if err != nil {
				t.Fatal(err)
			}
			defer func() { _ = store.Close() }()
			options := catalogOptions()
			catalog := mustCatalog(t, store, options)
			original := preparedEntry(t, store, "Content/Édit.uasset", bytes.Join([][]byte{block(1), block(2), block(3)}, nil))
			if err := catalog.RecordLocal(original); err != nil {
				t.Fatal(err)
			}
			// These names cannot be directly represented in a portable native
			// workspace. Their immutable catalog identity must still be lossless.
			for _, name := range []string{".git/config", "A", "a"} {
				entry := original
				entry.Info.Name = name
				if err := catalog.RecordLocal(entry); err != nil {
					t.Fatal(err)
				}
			}
			mustCommitCatalog(t, store, catalog)
			firstRevision := catalog.Revision()
			indexID, _, _ := catalog.LocalIndex()
			refs, err := store.Refs()
			if err != nil || len(refs) != 1 {
				t.Fatalf("per-file pin leaked: %v %v", refs, err)
			}
			// A second independently loaded draft cannot overwrite a newer frontier.
			stale := mustCatalog(t, store, options)
			base := localEntry(t, catalog, original.Info.Name)
			remote := preparedEntry(t, store, original.Info.Name, bytes.Join([][]byte{block(3), block(1), block(4)}, nil))
			remote.Info.Version = base.Info.Version.Update(options.Peers[0].Short())
			remote.Info.ModifiedBy = options.Peers[0].Short()
			remote.Info.Sequence = 1
			receiveCatalog(t, catalog, options, true, 0, 1, remote.Info)
			if err := catalog.Adopt(options.Peers[0], remote.Info.Name); err == nil {
				t.Fatal("unverified data was adopted")
			}
			if err := catalog.Attach(ctx, store, options.Peers[0], remote.Info.Name, original.Manifest); err == nil {
				t.Fatal("wrong content attached")
			}
			if err := catalog.Attach(ctx, store, options.Peers[0], norm.NFD.String(remote.Info.Name), remote.Manifest); err != nil {
				t.Fatal(err)
			}
			if err := catalog.Adopt(options.Peers[0], remote.Info.Name); err != nil {
				t.Fatal(err)
			}
			mustCommitCatalog(t, store, catalog)
			if err := stale.Commit(ctx, store); !errors.Is(err, ErrCatalogStale) {
				t.Fatalf("stale draft overwrote publication: %v", err)
			}
			if err := catalog.Commit(ctx, store); err != nil {
				t.Fatal(err)
			}
			// Edit independently offline at both devices. Neither version may be
			// silently selected; all downloaded bytes stay rooted after restart.
			local := preparedEntry(t, store, original.Info.Name, []byte("local offline edit"))
			if err := catalog.RecordLocal(local); err != nil {
				t.Fatal(err)
			}
			concurrent := preparedEntry(t, store, original.Info.Name, []byte("paired offline edit"))
			concurrent.Info.Version = remote.Info.Version.Update(options.Peers[0].Short())
			concurrent.Info.ModifiedBy = options.Peers[0].Short()
			concurrent.Info.Sequence = 2
			receiveCatalog(t, catalog, options, false, 1, 2, concurrent.Info)
			if err := catalog.Attach(ctx, store, options.Peers[0], concurrent.Info.Name, concurrent.Manifest); err != nil {
				t.Fatal(err)
			}
			before := localEntry(t, catalog, original.Info.Name)
			if err := catalog.Adopt(options.Peers[0], concurrent.Info.Name); !errors.Is(err, ErrCatalogConflict) {
				t.Fatalf("concurrent replacement not refused: %v", err)
			}
			if !reflect.DeepEqual(before, localEntry(t, catalog, original.Info.Name)) {
				t.Fatal("conflict changed local version")
			}
			mustCommitCatalog(t, store, catalog)
			// A causally dominating explicit deletion and a new path encode rename.
			deleted := concurrent.Info
			deleted.Version = deleted.Version.Merge(before.Info.Version).Update(options.Peers[0].Short())
			deleted.Sequence = 3
			deleted.Deleted = true
			deleted.Size = 0
			deleted.Blocks = nil
			deleted.BlocksHash = nil
			renamed := remote.Info
			renamed.Name = "Content/Renamed.uasset"
			renamed.Sequence = 4
			renamed.Version = deleted.Version
			renamed.ModifiedBy = options.Peers[0].Short()
			receiveCatalog(t, catalog, options, false, 2, 4, deleted, renamed)
			if err := catalog.Attach(ctx, store, options.Peers[0], renamed.Name, remote.Manifest); err != nil {
				t.Fatal(err)
			}
			for _, name := range []string{deleted.Name, renamed.Name} {
				if err := catalog.Adopt(options.Peers[0], name); err != nil {
					t.Fatal(err)
				}
			}
			mustCommitCatalog(t, store, catalog)
			finalRevision := catalog.Revision()
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}
			cli := os.Getenv("SYRNCK_TEST_CLI")
			if cli == "" {
				t.Fatal("catalog retention suite requires the built CLI")
			}
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
			store, err = syrnck.Open(root, syrnck.Options{Key: key})
			if err != nil {
				t.Fatal(err)
			}
			reopened := mustCatalog(t, store, options)
			actualIndex, sequence, _ := reopened.LocalIndex()
			if actualIndex != indexID || sequence != 8 || reopened.Revision() != finalRevision {
				t.Fatalf("restart lost frontier: %v %v", actualIndex, sequence)
			}
			if !localEntry(t, reopened, original.Info.Name).Info.Deleted {
				t.Fatal("tombstone lost on restart")
			}
			for _, entry := range []CatalogEntry{original, remote, local, concurrent} {
				if err := store.VerifyFile(entry.Manifest); err != nil {
					t.Fatalf("GC deleted retained historical bytes: %v", err)
				}
			}
			if _, err := store.Get(firstRevision); err != nil {
				t.Fatal("historical catalog was lost", err)
			}
			if _, err := store.Verify(); err != nil {
				t.Fatal(err)
			}
			changedConfig := options
			changedConfig.Peers = nil
			if _, err := OpenCatalog(ctx, store, changedConfig); err == nil {
				t.Fatal("different membership accepted")
			}
		})
	}
}

func TestCatalogIndexValidationAndOwnership(t *testing.T) {
	ctx := context.Background()
	store := openStore(t, false)
	options := catalogOptions()
	catalog := mustCatalog(t, store, options)
	entry := preparedEntry(t, store, "Content/Édit.uasset", []byte("verified bytes"))
	entry.Info.Sequence = 1
	entry.Info.ModifiedBy = options.Peers[0].Short()
	entry.Info.Version = protocol.Vector{}.Update(options.Peers[0].Short())
	input := copyInfo(entry.Info)
	input.Platform.Unix = &protocol.UnixData{OwnerName: "owner"}
	receiveCatalog(t, catalog, options, true, 0, 1, input)
	input.Version.Counters[0].Value++
	input.Blocks[0].Hash[0] ^= 255
	input.Platform.Unix.OwnerName = "mutated"
	_, _, snapshot, _ := catalog.PeerIndex(options.Peers[0])
	if snapshot[0].Info.Platform.Unix.OwnerName != "owner" || !snapshot[0].Info.Version.Equal(entry.Info.Version) || !bytes.Equal(snapshot[0].Info.Blocks[0].Hash, entry.Info.Blocks[0].Hash) {
		t.Fatal("caller mutated catalog state")
	}
	snapshot[0].Info.Platform.Unix.OwnerName = "snapshot mutation"
	_, _, again, _ := catalog.PeerIndex(options.Peers[0])
	if again[0].Info.Platform.Unix.OwnerName != "owner" {
		t.Fatal("snapshot aliases catalog")
	}
	base := cloneCatalogState(catalog.state)
	valid := entry.Info
	valid.Platform.Unix = &protocol.UnixData{OwnerName: "owner"}
	valid.Sequence = 2
	for _, test := range []struct {
		name   string
		change func(*CatalogIndex)
	}{
		{"unpaired", func(b *CatalogIndex) { b.Device = protocol.NewDeviceID([]byte("stranger")) }},
		{"changed generation", func(b *CatalogIndex) { b.IndexID = 24 }},
		{"sequence gap", func(b *CatalogIndex) { b.PrevSequence = 0 }},
		{"last mismatch", func(b *CatalogIndex) { b.LastSequence = 3 }},
		{"duplicate name", func(b *CatalogIndex) {
			second := copyInfo(b.Files[0])
			second.Sequence = 3
			b.Files = append(b.Files, second)
			b.LastSequence = 3
		}},
		{"duplicate vector", func(b *CatalogIndex) {
			b.Files[0].Version.Counters = append(b.Files[0].Version.Counters, b.Files[0].Version.Counters[0])
		}},
		{"equal vector different content", func(b *CatalogIndex) { b.Files[0].Blocks[0].Hash[0] ^= 255; b.Files[0].BlocksHash = nil }},
		{"unsafe name", func(b *CatalogIndex) { b.Files[0].Name = "../outside" }},
		{"invalid local flags", func(b *CatalogIndex) { b.Files[0].LocalFlags = 1 }},
	} {
		t.Run(test.name, func(t *testing.T) {
			batch := CatalogIndex{Device: options.Peers[0], IndexID: 23, PrevSequence: 1, LastSequence: 2, Files: []protocol.FileInfo{copyInfo(valid)}}
			test.change(&batch)
			if err := catalog.ReceiveIndex(batch); err == nil {
				t.Fatal("invalid announcement accepted")
			}
			if !reflect.DeepEqual(base, catalog.state) {
				t.Fatal("invalid announcement changed draft")
			}
		})
	}
	if err := catalog.Attach(ctx, store, options.Peers[0], entry.Info.Name, entry.Manifest); err != nil {
		t.Fatal(err)
	}
	mustCommitCatalog(t, store, catalog)
	// A new peer index generation requires a complete reset. The previous
	// verified generation remains native history even when its names disappear.
	if err := catalog.ReceiveIndex(CatalogIndex{Device: options.Peers[0], IndexID: 99, Reset: true}); err != nil {
		t.Fatal(err)
	}
	mustCommitCatalog(t, store, catalog)
	_, sequence, files, err := mustCatalog(t, store, options).PeerIndex(options.Peers[0])
	if err != nil || sequence != 0 || len(files) != 0 {
		t.Fatal("peer reset did not persist", err)
	}
}

func TestCatalogRefusesCorruptionOverflowAndCancelledPublication(t *testing.T) {
	store := openStore(t, true)
	options := catalogOptions()
	catalog := mustCatalog(t, store, options)
	entry := preparedEntry(t, store, "file", []byte("first"))
	if err := catalog.RecordLocal(entry); err != nil {
		t.Fatal(err)
	}
	mustCommitCatalog(t, store, catalog)
	before := catalog.Revision()
	entry = preparedEntry(t, store, "file", []byte("second"))
	if err := catalog.RecordLocal(entry); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := catalog.Commit(ctx, store); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled publication: %v", err)
	}
	if mustCatalog(t, store, options).Revision() != before {
		t.Fatal("cancelled publication changed durable root")
	}
	data, _ := json.Marshal(catalog.state)
	for _, corrupt := range [][]byte{append([]byte(`{"version":1,`), data[1:]...), append(bytes.Clone(data), ' '), bytes.Replace(data, []byte(`"version":1`), []byte(`"version":2,"unknown":true`), 1)} {
		var state catalogState
		if err := strictCatalogJSON(corrupt, &state); err == nil {
			t.Fatal("corrupt or ambiguous state accepted")
		}
	}
	bad := mustCatalog(t, store, options)
	bad.state.Sequence = math.MaxInt64
	if err := bad.RecordLocal(entry); err == nil {
		t.Fatal("sequence wrapped")
	}
	bad = mustCatalog(t, store, options)
	bad.state.Files[0].Info.Version.Counters[0].Value = math.MaxUint64
	if err := bad.RecordLocal(entry); err == nil {
		t.Fatal("local vector wrapped")
	}
	bad = mustCatalog(t, store, options)
	wrong := preparedEntry(t, store, "other", []byte("wrong"))
	bad.state.Files[0].Manifest = wrong.Manifest
	if err := bad.Commit(context.Background(), store); err == nil {
		t.Fatal("mismatched payload was rooted")
	}
	if mustCatalog(t, store, options).Revision() != before {
		t.Fatal("invalid data changed durable root")
	}
}

func TestCatalogScannerBatchIsAtomicAndPreservesMetadata(t *testing.T) {
	store := openStore(t, false)
	catalog := mustCatalog(t, store, catalogOptions())
	changes := make([]CatalogEntry, 1024)
	for i := range changes {
		changes[i].Info = protocol.FileInfo{Name: fmt.Sprintf("folder/%04d", 1023-i), Type: protocol.FileInfoTypeDirectory}
	}
	if err := catalog.RecordLocalChanges(changes); err != nil {
		t.Fatal(err)
	}
	_, sequence, files := catalog.LocalIndex()
	if sequence != 1024 || len(files) != 1024 || files[0].Info.Name != "folder/1023" || files[1023].Info.Name != "folder/0000" {
		t.Fatal("batch sequence order differs")
	}
	before, _ := json.Marshal(catalog.state)
	bad := append([]CatalogEntry(nil), changes[:2]...)
	bad[1].Info.Name = bad[0].Info.Name
	if err := catalog.RecordLocalChanges(bad); err == nil {
		t.Fatal("duplicate batch accepted")
	}
	after, _ := json.Marshal(catalog.state)
	if !bytes.Equal(before, after) {
		t.Fatal("failed batch partially changed draft")
	}
	link := CatalogEntry{Info: protocol.FileInfo{Name: "link", Type: protocol.FileInfoTypeSymlink, SymlinkTarget: []byte("../outside-is-metadata-only")}}
	if err := catalog.RecordLocal(link); err != nil {
		t.Fatal(err)
	}
	changes[0].Info.Name = "caller mutation"
	link.Info.SymlinkTarget[0] = 'X'
	if string(localEntry(t, catalog, "link").Info.SymlinkTarget) != "../outside-is-metadata-only" {
		t.Fatal("symlink metadata aliases caller")
	}
	mustCommitCatalog(t, store, catalog)
	reopened := mustCatalog(t, store, catalogOptions())
	if !bytes.Equal(localEntry(t, reopened, "link").Info.SymlinkTarget, []byte("../outside-is-metadata-only")) {
		t.Fatal("symlink descriptor lost")
	}
}

func BenchmarkCatalogScannerBatch(b *testing.B) {
	for _, count := range []int{1000, 10000} {
		b.Run(fmt.Sprint(count), func(b *testing.B) {
			state, err := catalogConfiguration(catalogOptions())
			if err != nil {
				b.Fatal(err)
			}
			state.IndexID = 1
			changes := make([]CatalogEntry, count)
			for i := range changes {
				changes[i].Info = protocol.FileInfo{Name: fmt.Sprintf("folder/%06d", count-i), Type: protocol.FileInfoTypeDirectory}
			}
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				catalog := &Catalog{state: state}
				if err := catalog.RecordLocalChanges(changes); err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}
