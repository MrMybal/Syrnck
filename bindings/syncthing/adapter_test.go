package syncthing

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
	"github.com/syncthing/syncthing/lib/scanner"
	"golang.org/x/text/unicode/norm"
	syrnck "syrnck.local/bindings/go"
)

func openStore(t testing.TB, encrypted bool) *syrnck.Store {
	t.Helper()
	var key []byte
	if encrypted {
		key = bytes.Repeat([]byte{57}, 32)
	}
	store, err := syrnck.Open(filepath.Join(tempDirectory(t), "store"), syrnck.Options{Create: true, Key: key})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := store.Close(); err != nil {
			t.Error(err)
		}
	})
	return store
}

func ingest(t testing.TB, store *syrnck.Store, data []byte, fixed bool) string {
	t.Helper()
	name := filepath.Join(tempDirectory(t), "source")
	if err := os.WriteFile(name, data, 0600); err != nil {
		t.Fatal(err)
	}
	var chunking syrnck.Chunking
	if fixed {
		chunking = syrnck.Chunking{Fixed: true, Min: 65536, Average: 65536, Max: 65536}
	}
	id, err := store.Ingest(name, chunking)
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func file(t testing.TB, store *syrnck.Store, manifest string) *File {
	t.Helper()
	f, err := NewFile(context.Background(), store, manifest, "project", "Content/Édit.uasset", protocol.MinBlockSize)
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func block(value byte) []byte { return bytes.Repeat([]byte{value}, protocol.MinBlockSize) }

func TestReferenceScannerAndDifferentialReconstruction(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(map[bool]string{false: "plain", true: "encrypted"}[encrypted], func(t *testing.T) {
			source, destination := openStore(t, encrypted), openStore(t, encrypted)
			before := bytes.Join([][]byte{block(1), block(2), block(1), block(3), []byte("tail")}, nil)
			after := bytes.Join([][]byte{block(3), block(1), block(1), block(4), []byte("tail")}, nil)
			original := ingest(t, destination, before, true)
			incoming := file(t, source, ingest(t, source, after, false))
			oracle, err := scanner.Blocks(context.Background(), bytes.NewReader(after), protocol.MinBlockSize, int64(len(after)), nil)
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(oracle, incoming.Info().Blocks) {
				t.Fatal("index differs from Syncthing scanner")
			}
			result, err := Fetch(context.Background(), destination, "project", incoming.Info(), incoming.ReadRequest,
				FetchOptions{ReuseManifest: original})
			if err != nil {
				t.Fatal(err)
			}
			if result.Stats.Requests != 1 || result.Stats.TransferredBytes != protocol.MinBlockSize {
				t.Fatalf("unexpected transfer: %+v", result.Stats)
			}
			restored := filepath.Join(tempDirectory(t), "restored")
			if err := destination.Restore(result.Manifest, restored); err != nil {
				t.Fatal(err)
			}
			got, err := os.ReadFile(restored)
			if err != nil || !bytes.Equal(got, after) {
				t.Fatal("differential reconstruction differs", err)
			}
			if _, err := destination.Verify(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestResumeValidatesPersistedRangesAndRefusesAnotherIndex(t *testing.T) {
	source, destination := openStore(t, false), openStore(t, true)
	data := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	incoming := file(t, source, ingest(t, source, data, false))
	resume := filepath.Join(tempDirectory(t), "resume")
	ctx, cancel := context.WithCancel(context.Background())
	calls := 0
	request := func(ctx context.Context, req *protocol.Request) ([]byte, error) {
		calls++
		if calls == 2 {
			cancel()
			return nil, context.Canceled
		}
		return incoming.ReadRequest(ctx, req)
	}
	interrupted, err := Fetch(ctx, destination, "project", incoming.Info(), request, FetchOptions{ResumeDirectory: resume})
	if !errors.Is(err, context.Canceled) || interrupted.Manifest != "" {
		t.Fatal("interrupted transfer was published", err)
	}
	result, err := Fetch(context.Background(), destination, "project", incoming.Info(), incoming.ReadRequest, FetchOptions{ResumeDirectory: resume})
	if err != nil || result.Stats.Requests != 2 || result.Stats.StagedBytes != protocol.MinBlockSize {
		t.Fatalf("resume failed: %+v %v", result, err)
	}
	// A corrupt staged range is re-requested instead of trusted from a receipt.
	stageData := filepath.Join(resume, "data")
	bytesOnDisk, err := os.ReadFile(stageData)
	if err != nil {
		t.Fatal(err)
	}
	bytesOnDisk[0] ^= 1
	if err := os.WriteFile(stageData, bytesOnDisk, 0600); err != nil {
		t.Fatal(err)
	}
	fixed, err := Fetch(context.Background(), destination, "project", incoming.Info(), incoming.ReadRequest, FetchOptions{ResumeDirectory: resume})
	if err != nil || fixed.Stats.Requests != 1 || fixed.Manifest != result.Manifest {
		t.Fatal("staged corruption was not repaired", err)
	}
	state, _ := os.ReadFile(filepath.Join(resume, "state.json"))
	changed := incoming.Info()
	changed.Blocks[0].Hash[0] ^= 1
	changed.BlocksHash = protocol.BlocksHash(changed.Blocks)
	if _, err := Fetch(context.Background(), destination, "project", changed, incoming.ReadRequest, FetchOptions{ResumeDirectory: resume}); err == nil {
		t.Fatal("different index reused staging")
	}
	afterState, _ := os.ReadFile(filepath.Join(resume, "state.json"))
	if !bytes.Equal(state, afterState) {
		t.Fatal("refused index replaced resume metadata")
	}
}

func TestInvalidPeerDataAndRequestsNeverPublishPartialFiles(t *testing.T) {
	source, destination := openStore(t, false), openStore(t, false)
	incoming := file(t, source, ingest(t, source, block(9), false))
	before, err := destination.Verify()
	if err != nil {
		t.Fatal(err)
	}
	bad := func(_ context.Context, req *protocol.Request) ([]byte, error) { return make([]byte, req.Size), nil }
	if result, err := Fetch(context.Background(), destination, "project", incoming.Info(), bad, FetchOptions{}); err == nil || result.Manifest != "" {
		t.Fatal("corrupt peer data accepted")
	}
	after, err := destination.Verify()
	if err != nil || before != after {
		t.Fatal("corrupt transfer wrote engine objects", err)
	}
	info := incoming.Info()
	for _, update := range []func(*protocol.FileInfo){
		func(f *protocol.FileInfo) { f.RawBlockSize = 65536 }, func(f *protocol.FileInfo) { f.Blocks[0].Offset = 1 },
		func(f *protocol.FileInfo) { f.Blocks[0].Hash = nil }, func(f *protocol.FileInfo) { f.Name = "../escape" },
		func(f *protocol.FileInfo) { f.Deleted = true }, func(f *protocol.FileInfo) { f.Size-- },
	} {
		changed := copyInfo(info)
		update(&changed)
		if _, err := Fetch(context.Background(), destination, "project", changed, incoming.ReadRequest, FetchOptions{}); err == nil {
			t.Fatal("malformed index accepted")
		}
	}
	req := &protocol.Request{Folder: "project", Name: info.Name, Size: info.Blocks[0].Size, Hash: bytes.Clone(info.Blocks[0].Hash)}
	if _, err := incoming.ReadRequest(context.Background(), req); err != nil {
		t.Fatal(err)
	}
	req.Hash = nil
	if _, err := incoming.ReadRequest(context.Background(), req); err != nil {
		t.Fatal("optional hash rejected", err)
	}
	req.Folder = "other"
	if _, err := incoming.ReadRequest(context.Background(), req); err == nil {
		t.Fatal("other folder was served")
	}
	// The caller's copy cannot mutate the served immutable index.
	info.Blocks[0].Hash[0] ^= 1
	req.Folder = "project"
	if _, err := incoming.ReadRequest(context.Background(), req); err != nil {
		t.Fatal(err)
	}
}

func TestEmptyFileReferenceCompatibility(t *testing.T) {
	source, destination := openStore(t, false), openStore(t, false)
	incoming := file(t, source, ingest(t, source, nil, false))
	oracle, err := scanner.Blocks(context.Background(), bytes.NewReader(nil), protocol.MinBlockSize, 0, nil)
	if err != nil || !reflect.DeepEqual(oracle, incoming.Info().Blocks) {
		t.Fatal("empty-file convention differs", err)
	}
	result, err := Fetch(context.Background(), destination, "project", incoming.Info(), nil, FetchOptions{})
	if err != nil || result.Stats.Requests != 0 {
		t.Fatal("empty file requested data", err)
	}
}

func TestNativeUnicodeNamesKeepCanonicalBEPIdentity(t *testing.T) {
	source, destination := openStore(t, true), openStore(t, true)
	data := bytes.Join([][]byte{block(31), block(32)}, nil)
	nfc := "Content/Édit.uasset"
	native := norm.NFD.String(nfc)
	view, err := NewFile(context.Background(), source, ingest(t, source, data, false), "project", native, protocol.MinBlockSize)
	if err != nil {
		t.Fatal(err)
	}
	info := view.Info()
	if info.Name != nfc {
		t.Fatal("native filename changed the immutable protocol identity")
	}
	first := info.Blocks[0]
	got, err := view.ReadRequest(context.Background(), &protocol.Request{Folder: "project", Name: native,
		Offset: first.Offset, Size: first.Size, Hash: first.Hash})
	if err != nil || !bytes.Equal(got, data[:first.Size]) {
		t.Fatalf("native NFD block request refused: %v", err)
	}
	info.Name = native
	result, err := Fetch(context.Background(), destination, "project", info,
		func(ctx context.Context, request *protocol.Request) ([]byte, error) {
			if request.Name != nfc {
				t.Error("outgoing request was not canonical NFC")
			}
			return view.ReadRequest(ctx, request)
		}, FetchOptions{})
	if err != nil {
		t.Fatal(err)
	}
	actual, err := destination.ReadRange(result.Manifest, 0, uint64(len(data)))
	if err != nil || !bytes.Equal(actual, data) || info.Name != native {
		t.Fatal("native normalization changed data or caller index")
	}
}
