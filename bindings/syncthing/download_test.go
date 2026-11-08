package syncthing

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func TestDownloadedFileReleasesStoreDuringNetworkAndRevalidatesImport(t *testing.T) {
	ctx := context.Background()
	source := openStore(t, false)
	before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
	incoming := file(t, source, ingest(t, source, after, false))
	root := filepath.Join(tempDirectory(t), "destination")
	store, err := syrnck.Open(root, syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	old := preparedEntry(t, store, incoming.info.Name, before)
	if err := store.Pin("download/reuse", syrnck.ObjectKey{Kind: "manifest", ID: old.Manifest}); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	request := func(ctx context.Context, req *protocol.Request) ([]byte, error) {
		// Taking the exclusive native lock while the remote call is outstanding
		// proves DownloadFile does not hold a destination Store behind our back.
		store, err := syrnck.Open(root, syrnck.Options{})
		if err != nil {
			t.Fatal("native store locked during network wait", err)
		}
		if _, err := store.Refs(); err != nil {
			t.Fatal(err)
		}
		if err := store.Close(); err != nil {
			t.Fatal(err)
		}
		return incoming.ReadRequest(ctx, req)
	}
	reuse := func(ctx context.Context, req *protocol.Request) ([]byte, error) {
		store, err := syrnck.Open(root, syrnck.Options{})
		if err != nil {
			return nil, err
		}
		defer store.Close()
		view := &File{store: store, manifest: old.Manifest, folder: "project", info: old.Info}
		return view.ReadRequest(ctx, req)
	}
	staged, err := DownloadFile(ctx, "project", incoming.Info(), request, DownloadOptions{ReuseIndex: &old.Info, ReadReuse: reuse})
	if err != nil {
		t.Fatal(err)
	}
	defer staged.Close()
	if staged.Stats().Requests != 1 || staged.Stats().ReusedBytes != 2*uint64(protocol.MinBlockSize) {
		t.Fatalf("differential staging: %+v", staged.Stats())
	}
	store, err = syrnck.Open(root, syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	refsBefore, _ := store.Refs()
	result, err := staged.Prepare(ctx, store)
	if err != nil {
		t.Fatal(err)
	}
	refsAfter, _ := store.Refs()
	if len(refsAfter) != len(refsBefore) {
		t.Fatal("unretained download created roots")
	}
	actual, err := store.ReadRange(result.Manifest, 0, uint64(len(after)))
	if err != nil || !bytes.Equal(actual, after) {
		t.Fatal("download bytes differ", err)
	}
	// An outside writer changing a private stage after download must never
	// bind different bytes to the previously verified announced index.
	if err := os.WriteFile(staged.path, []byte("changed after download"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := staged.Prepare(ctx, store); err == nil {
		t.Fatal("mutated stage was accepted")
	}
	if err := staged.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := staged.Prepare(ctx, store); err == nil {
		t.Fatal("closed stage imported")
	}
	if _, err := os.Stat(staged.path); !os.IsNotExist(err) {
		t.Fatal("ephemeral stage leaked", err)
	}
}
