package syncthing

import (
	"bytes"
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func folderTestOptions(root string, local, peer protocol.DeviceID, encrypted bool) FolderOptions {
	var key []byte
	if encrypted {
		key = bytes.Repeat([]byte{83}, 32)
	}
	storage := filepath.Join(root, ".syrnck")
	return FolderOptions{Catalog: CatalogOptions{Folder: "project", LocalDevice: local, Peers: []protocol.DeviceID{peer}}, Directory: root, Storage: storage,
		OpenStore: func(ctx context.Context) (*syrnck.Store, error) {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			return syrnck.Open(storage, syrnck.Options{Key: key})
		}}
}

func createTestFolder(t *testing.T, options FolderOptions, files map[string][]byte) *Folder {
	t.Helper()
	if err := os.MkdirAll(options.Directory, 0700); err != nil {
		t.Fatal(err)
	}
	for name, data := range files {
		path := filepath.Join(options.Directory, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	var key []byte
	// The test provider below uses this deterministic test-only key.
	if strings.Contains(filepath.Base(options.Directory), "encrypted") {
		key = bytes.Repeat([]byte{83}, 32)
	}
	store, err := syrnck.Open(options.Storage, syrnck.Options{Create: true, Key: key})
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	folder, err := NewFolder(context.Background(), options)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := folder.Close(); err != nil {
			t.Error(err)
		}
	})
	return folder
}

func folderSnapshot(t *testing.T, folder *Folder) FolderSnapshot {
	t.Helper()
	snapshot, err := folder.Snapshot(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	return snapshot
}

func exchangeFolderIndex(t *testing.T, source, destination *Folder, reset bool, previous int64) FolderSnapshot {
	t.Helper()
	snapshot := folderSnapshot(t, source)
	var files []protocol.FileInfo
	for _, entry := range snapshot.Files {
		if reset || entry.Info.Sequence > previous {
			files = append(files, entry.Info)
		}
	}
	err := destination.ReceiveIndex(context.Background(), CatalogIndex{Device: source.options.Catalog.LocalDevice, IndexID: snapshot.IndexID,
		Reset: reset, PrevSequence: previous, LastSequence: snapshot.Sequence, Files: files})
	if err != nil {
		t.Fatal(err)
	}
	return snapshot
}

func snapshotRequester(t *testing.T, folder *Folder, snapshot FolderSnapshot, onRequest func()) RequestFunc {
	t.Helper()
	files := make(map[string]CatalogEntry, len(snapshot.Files))
	for _, entry := range snapshot.Files {
		files[entry.Info.Name] = entry
	}
	return func(ctx context.Context, req *protocol.Request) ([]byte, error) {
		if onRequest != nil {
			onRequest()
		}
		entry, ok := files[protocolName(req.Name)]
		if !ok || entry.Info.Deleted {
			return nil, protocol.ErrNoSuchFile
		}
		return folder.readEntry(ctx, entry, req)
	}
}

func TestFolderControllerBidirectionalWorkspaceChangesAndGC(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			ctx := context.Background()
			root := tempDirectory(t)
			suffix := "plain"
			if encrypted {
				suffix = "encrypted"
			}
			leftID := protocol.NewDeviceID([]byte("folder controller left"))
			rightID := protocol.NewDeviceID([]byte("folder controller right"))
			leftOptions := folderTestOptions(filepath.Join(root, "left-"+suffix), leftID, rightID, encrypted)
			rightOptions := folderTestOptions(filepath.Join(root, "right-"+suffix), rightID, leftID, encrypted)
			before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
			after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
			name := "Content/Édit.uasset"
			renamed := "Content/Renamed.uasset"
			left := createTestFolder(t, leftOptions, map[string][]byte{name: before})
			right := createTestFolder(t, rightOptions, nil)
			initial := exchangeFolderIndex(t, left, right, true, 0)
			requests := 0
			status, err := right.Step(ctx, func(device protocol.DeviceID) RequestFunc {
				if device != leftID {
					t.Fatal("wrong requester identity")
				}
				return snapshotRequester(t, left, initial, func() { requests++ })
			})
			if err != nil || requests != 3 || status.Transfer.TransferredBytes != uint64(len(before)) {
				t.Fatalf("initial folder pull: %+v requests=%d %v", status, requests, err)
			}
			actual, err := os.ReadFile(filepath.Join(rightOptions.Directory, filepath.FromSlash(name)))
			if err != nil || !bytes.Equal(actual, before) {
				t.Fatal("native working folder was not reconstructed", err)
			}
			echo := exchangeFolderIndex(t, right, left, true, 0)
			if _, err := left.Step(ctx, nil); err != nil {
				t.Fatal("equal verified stream needed network or conflicted", err)
			}
			if err := os.WriteFile(filepath.Join(rightOptions.Directory, filepath.FromSlash(name)), after, 0600); err != nil {
				t.Fatal(err)
			}
			if err := right.Invalidate(); err != nil {
				t.Fatal(err)
			}
			if _, err := right.Step(ctx, nil); err != nil {
				t.Fatal(err)
			}
			changed := exchangeFolderIndex(t, right, left, false, echo.Sequence)
			requests = 0
			status, err = left.Step(ctx, func(protocol.DeviceID) RequestFunc {
				return snapshotRequester(t, right, changed, func() {
					requests++
					// The destination's exclusive Store is available during the network
					// callback; no native handle is kept open across a remote wait.
					store, err := leftOptions.OpenStore(ctx)
					if err != nil {
						t.Fatal("destination locked while requesting data", err)
					}
					if err := store.Close(); err != nil {
						t.Fatal(err)
					}
				})
			})
			if err != nil || requests != 1 || status.Transfer.ReusedBytes != 2*uint64(protocol.MinBlockSize) {
				t.Fatalf("differential workspace pull: %+v requests=%d %v", status, requests, err)
			}
			actual, err = os.ReadFile(filepath.Join(leftOptions.Directory, filepath.FromSlash(name)))
			if err != nil || !bytes.Equal(actual, after) {
				t.Fatal("updated working bytes differ", err)
			}
			if err := os.Rename(filepath.Join(rightOptions.Directory, filepath.FromSlash(name)), filepath.Join(rightOptions.Directory, filepath.FromSlash(renamed))); err != nil {
				t.Fatal(err)
			}
			if err := right.Invalidate(); err != nil {
				t.Fatal(err)
			}
			if _, err := right.Step(ctx, nil); err != nil {
				t.Fatal(err)
			}
			exchangeFolderIndex(t, right, left, false, changed.Sequence)
			if _, err := left.Step(ctx, nil); err != nil {
				t.Fatal("exact rename transferred data or lost its tombstone", err)
			}
			if _, err := os.Stat(filepath.Join(leftOptions.Directory, filepath.FromSlash(name))); !os.IsNotExist(err) {
				t.Fatal("old working path remains", err)
			}
			actual, err = os.ReadFile(filepath.Join(leftOptions.Directory, filepath.FromSlash(renamed)))
			if err != nil || !bytes.Equal(actual, after) {
				t.Fatal("rename lost data", err)
			}
			final := folderSnapshot(t, left)
			var oldManifest string
			for _, entry := range initial.Files {
				if entry.Info.Name == name {
					oldManifest = entry.Manifest
				}
			}
			if err := left.Close(); err != nil {
				t.Fatal(err)
			}
			cli := os.Getenv("SYRNCK_TEST_CLI")
			if cli == "" {
				t.Fatal("folder retention suite requires built CLI")
			}
			command := exec.Command(cli, "--store", leftOptions.Storage, "gc", "--delete")
			for _, entry := range os.Environ() {
				if !strings.HasPrefix(strings.ToUpper(entry), "SYRNCK_KEY=") {
					command.Env = append(command.Env, entry)
				}
			}
			if encrypted {
				command.Env = append(command.Env, "SYRNCK_KEY="+hex.EncodeToString(bytes.Repeat([]byte{83}, 32)))
			}
			if output, err := command.CombinedOutput(); err != nil {
				t.Fatalf("folder history GC: %v %s", err, output)
			}
			reopened, err := NewFolder(ctx, leftOptions)
			if err != nil {
				t.Fatal(err)
			}
			defer reopened.Close()
			if folderSnapshot(t, reopened).Sequence != final.Sequence {
				t.Fatal("restart reset advertised sequence")
			}
			store, err := leftOptions.OpenStore(ctx)
			if err != nil {
				t.Fatal(err)
			}
			defer store.Close()
			if err := store.VerifyFile(oldManifest); err != nil {
				t.Fatal("GC discarded initial complete file", err)
			}
			refs, err := store.Refs()
			if err != nil {
				t.Fatal(err)
			}
			for name := range refs {
				if strings.HasPrefix(name, "ffi/") && !strings.HasPrefix(name, "ffi/bep/") {
					t.Fatalf("per-file retained root leaked: %s", name)
				}
			}
			if _, err := store.Verify(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestFolderControllerRefusesStaleDownload(t *testing.T) {
	ctx := context.Background()
	root := tempDirectory(t)
	leftID := protocol.NewDeviceID([]byte("changing source"))
	rightID := protocol.NewDeviceID([]byte("changing receiver"))
	left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left-plain"), leftID, rightID, false), map[string][]byte{"asset": block(6)})
	right := createTestFolder(t, folderTestOptions(filepath.Join(root, "right-plain"), rightID, leftID, false), nil)
	old := exchangeFolderIndex(t, left, right, true, 0)
	changed := false
	_, err := right.Step(ctx, func(protocol.DeviceID) RequestFunc {
		return snapshotRequester(t, left, old, func() {
			if changed {
				return
			}
			changed = true
			if err := os.WriteFile(filepath.Join(left.options.Directory, "asset"), block(7), 0600); err != nil {
				t.Fatal(err)
			}
			if err := left.Invalidate(); err != nil {
				t.Fatal(err)
			}
			if _, err := left.Step(ctx, nil); err != nil {
				t.Fatal(err)
			}
			exchangeFolderIndex(t, left, right, false, old.Sequence)
		})
	})
	if !errors.Is(err, ErrFolderChanged) {
		t.Fatalf("advanced peer did not invalidate download: %v", err)
	}
	if _, err := os.Stat(filepath.Join(right.options.Directory, "asset")); !os.IsNotExist(err) {
		t.Fatal("stale data was installed", err)
	}
	latest := folderSnapshot(t, left)
	if _, err := right.Step(ctx, func(protocol.DeviceID) RequestFunc { return snapshotRequester(t, left, latest, nil) }); err != nil {
		t.Fatal(err)
	}
	actual, err := os.ReadFile(filepath.Join(right.options.Directory, "asset"))
	if err != nil || !bytes.Equal(actual, block(7)) {
		t.Fatal("current peer version was not installed", err)
	}
}

// Retain a prepared application without running its native journal. This is
// the same private boundary used by Step, not a production failure hook.
func prepareFolderApplication(t *testing.T, source, destination *Folder, name string) {
	t.Helper()
	snapshot := exchangeFolderIndex(t, source, destination, true, 0)
	var incoming CatalogEntry
	for _, entry := range snapshot.Files {
		if entry.Info.Name == name {
			incoming = entry
		}
	}
	ctx := context.Background()
	staged, err := DownloadFile(ctx, source.options.Catalog.Folder, incoming.Info, snapshotRequester(t, source, snapshot, nil), DownloadOptions{})
	if err != nil {
		t.Fatal(err)
	}
	defer staged.Close()
	err = destination.withStore(ctx, func(store *syrnck.Store) error {
		loaded, err := destination.load(ctx, store)
		if err != nil {
			return err
		}
		data, err := staged.PrepareWithChunking(ctx, store, syrnck.Chunking{})
		if err != nil {
			return err
		}
		if err = loaded.catalog.Attach(ctx, store, source.options.Catalog.LocalDevice, name, data.Manifest); err != nil {
			return err
		}
		if err = loaded.catalog.Commit(ctx, store); err != nil {
			return err
		}
		loaded, err = destination.load(ctx, store)
		if err != nil {
			return err
		}
		missing, draft, err := destination.selectIncoming(ctx, store, loaded)
		if err != nil {
			return err
		}
		if missing != nil {
			t.Fatal("prepared application still needs data")
		}
		return destination.prepareApplication(ctx, store, loaded, draft)
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = destination.Snapshot(ctx); !errors.Is(err, ErrFolderPending) {
		t.Fatal("prepared protocol checkpoint announced before application", err)
	}
}

func TestFolderControllerRetiresOnlyUnstartedIntent(t *testing.T) {
	ctx := context.Background()
	root := tempDirectory(t)
	leftID, rightID := protocol.NewDeviceID([]byte("intent left")), protocol.NewDeviceID([]byte("intent right"))
	left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left"), leftID, rightID, false), map[string][]byte{"asset": []byte("incoming")})
	right := createTestFolder(t, folderTestOptions(filepath.Join(root, "right"), rightID, leftID, false), nil)
	prepareFolderApplication(t, left, right, "asset")
	if err := os.WriteFile(filepath.Join(right.options.Directory, "asset"), []byte("new local work"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := right.Refresh(ctx); err == nil {
		t.Fatal("obstructed checkout succeeded")
	}
	if _, err := right.Refresh(ctx); err != nil {
		t.Fatal("unstarted intent prevented local capture", err)
	}
	data, err := os.ReadFile(filepath.Join(right.options.Directory, "asset"))
	if err != nil || string(data) != "new local work" {
		t.Fatal("unstarted intent overwrote local work", err)
	}
	if _, err := right.Snapshot(ctx); err != nil {
		t.Fatal("unstarted intent remains pending", err)
	}
}

func TestFolderControllerRequiresOwnedCompletionEvidence(t *testing.T) {
	ctx := context.Background()
	root := tempDirectory(t)
	leftID, rightID := protocol.NewDeviceID([]byte("proof left")), protocol.NewDeviceID([]byte("proof right"))
	left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left"), leftID, rightID, false), map[string][]byte{"asset": []byte("incoming")})
	right := createTestFolder(t, folderTestOptions(filepath.Join(root, "right"), rightID, leftID, false), nil)
	prepareFolderApplication(t, left, right, "asset")
	err := right.withStore(ctx, func(store *syrnck.Store) error {
		loaded, err := right.load(ctx, store)
		if err != nil {
			return err
		}
		// A manual writer advances HEAD to the same candidate without our
		// completed owner receipt. Even identical bytes do not prove our ACK.
		candidate := syrnck.ObjectKey{Kind: "revision", ID: loaded.pending.Candidate}
		if _, err = store.CompareRefsContext(ctx, []syrnck.RefUpdate{{Name: loaded.native.Reference, Expected: loaded.native.Revision, Value: &candidate}}); err != nil {
			return err
		}
		return os.WriteFile(filepath.Join(right.options.Directory, "asset"), []byte("incoming"), 0600)
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := right.Refresh(ctx); !errors.Is(err, ErrFolderPending) {
		t.Fatal("manual HEAD movement acknowledged owned publication", err)
	}
}

func TestFolderControllerPreservesDecomposedNativeNames(t *testing.T) {
	ctx := context.Background()
	root := tempDirectory(t)
	leftID, rightID := protocol.NewDeviceID([]byte("normalization left")), protocol.NewDeviceID([]byte("normalization right"))
	physical, canonical := "Content/E\u0301dit.uasset", "Content/Édit.uasset"
	left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left"), leftID, rightID, false), map[string][]byte{physical: []byte("first")})
	right := createTestFolder(t, folderTestOptions(filepath.Join(root, "right"), rightID, leftID, false), nil)
	initial := exchangeFolderIndex(t, left, right, true, 0)
	if len(initial.Files) != 1 || initial.Files[0].Info.Name != canonical {
		t.Fatal("native name was not canonicalized", initial.Files)
	}
	if _, err := right.Step(ctx, func(protocol.DeviceID) RequestFunc { return snapshotRequester(t, left, initial, nil) }); err != nil {
		t.Fatal(err)
	}
	echo := exchangeFolderIndex(t, right, left, true, 0)
	if _, err := left.Step(ctx, nil); err != nil {
		t.Fatal("decomposed native echo conflicted", err)
	}
	if err := os.WriteFile(filepath.Join(right.options.Directory, filepath.FromSlash(canonical)), []byte("new peer edit"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := right.Invalidate(); err != nil {
		t.Fatal(err)
	}
	if _, err := right.Refresh(ctx); err != nil {
		t.Fatal(err)
	}
	updated := exchangeFolderIndex(t, right, left, false, echo.Sequence)
	if _, err := left.Step(ctx, func(protocol.DeviceID) RequestFunc { return snapshotRequester(t, right, updated, nil) }); err != nil {
		t.Fatal(err)
	}
	actual, err := os.ReadFile(filepath.Join(left.options.Directory, filepath.FromSlash(physical)))
	if err != nil || string(actual) != "new peer edit" {
		t.Fatal("decomposed working spelling lost", err)
	}
	if err = left.Invalidate(); err != nil {
		t.Fatal(err)
	}
	if _, err = left.Refresh(ctx); err != nil {
		t.Fatal("decomposed scan invented tombstone", err)
	}
	final := folderSnapshot(t, left)
	if len(final.Files) != 1 || final.Files[0].Info.Deleted || final.Files[0].Info.Name != canonical {
		t.Fatal("decomposed spelling produced false deletion", final.Files)
	}
	if _, err = nativeProtocolNames(map[string]catalogRevisionFile{physical: {}, canonical: {}}); err == nil {
		t.Fatal("native aliases silently coalesced")
	}
}

func TestFolderPreservesEditsDuringNativeApplication(t *testing.T) {
	helper := os.Getenv("SYRNCK_TEST_CHECKOUT_EDIT_HELPER")
	if helper == "" {
		t.Skip("build syrnck_checkout_edit_tests to qualify native callback edit recovery")
	}
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			ctx := context.Background()
			root := tempDirectory(t)
			leftID, rightID := protocol.NewDeviceID([]byte("active edit left")), protocol.NewDeviceID([]byte("active edit right"))
			suffix := "plain"
			if encrypted {
				suffix = "encrypted"
			}
			left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left-"+suffix), leftID, rightID, encrypted), map[string][]byte{"asset": []byte("incoming")})
			options := folderTestOptions(filepath.Join(root, "right-"+suffix), rightID, leftID, encrypted)
			options.PreserveCheckoutEdits = true
			right := createTestFolder(t, options, nil)
			prepareFolderApplication(t, left, right, "asset")
			var pending folderPending
			if err := right.withStore(ctx, func(store *syrnck.Store) error {
				loaded, err := right.load(ctx, store)
				if err == nil {
					pending = *loaded.pending
				}
				return err
			}); err != nil {
				t.Fatal(err)
			}
			command := exec.Command(helper, "interrupt-folder", options.Directory, options.Storage, pending.Expected.Target, pending.Candidate, pending.Owner, "asset", suffix)
			if output, err := command.CombinedOutput(); err != nil {
				t.Fatalf("native callback helper: %v %s", err, output)
			}
			if err := right.Close(); err != nil {
				t.Fatal(err)
			}
			// The in-flight policy is durable: a new process must resume that same
			// operation rather than silently adopting a changed future configuration.
			options.PreserveCheckoutEdits = false
			reopened, err := NewFolder(ctx, options)
			if err != nil {
				t.Fatal(err)
			}
			defer reopened.Close()
			if _, err = reopened.Refresh(ctx); err != nil {
				t.Fatal("automatic native edit preservation", err)
			}
			snapshot := folderSnapshot(t, reopened)
			if len(snapshot.Files) != 2 {
				t.Fatal("missing preserved local conflict file", snapshot.Files)
			}
			var copyName string
			for _, entry := range snapshot.Files {
				if entry.Info.Name != "asset" {
					copyName = entry.Info.Name
				}
			}
			if !strings.Contains(copyName, ".syrnck-conflict-") {
				t.Fatal("invalid preserved file name", copyName)
			}
			expected := []byte("edited during live native application")
			actual, err := os.ReadFile(filepath.Join(options.Directory, filepath.FromSlash(copyName)))
			if err != nil || !bytes.Equal(actual, expected) {
				t.Fatal("live edit bytes lost", err)
			}
			exchangeFolderIndex(t, reopened, left, true, 0)
			if _, err = left.Step(ctx, func(protocol.DeviceID) RequestFunc { return snapshotRequester(t, reopened, snapshot, nil) }); err != nil {
				t.Fatal(err)
			}
			actual, err = os.ReadFile(filepath.Join(left.options.Directory, filepath.FromSlash(copyName)))
			if err != nil || !bytes.Equal(actual, expected) {
				t.Fatal("preserved file was not synchronized", err)
			}
		})
	}
}
