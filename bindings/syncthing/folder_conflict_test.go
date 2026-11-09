package syncthing

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
)

func sendFrozenIndex(t *testing.T, source, destination *Folder, snapshot FolderSnapshot) {
	t.Helper()
	files := make([]protocol.FileInfo, 0, len(snapshot.Files))
	for _, entry := range snapshot.Files {
		files = append(files, entry.Info)
	}
	if err := destination.ReceiveIndex(context.Background(), CatalogIndex{Device: source.options.Catalog.LocalDevice, IndexID: snapshot.IndexID,
		Reset: true, LastSequence: snapshot.Sequence, Files: files}); err != nil {
		t.Fatal(err)
	}
}

func TestFoldersResolveConcurrentAssetsWithoutOscillation(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		for _, same := range []bool{false, true} {
			t.Run(fmt.Sprintf("%v/same=%v", encrypted, same), func(t *testing.T) {
				ctx := context.Background()
				root := tempDirectory(t)
				suffix := "plain"
				if encrypted {
					suffix = "encrypted"
				}
				leftID, rightID := protocol.NewDeviceID([]byte("conflict left")), protocol.NewDeviceID([]byte("conflict right"))
				name := "Content/Édit.uasset"
				leftOptions := folderTestOptions(filepath.Join(root, "left-"+suffix), leftID, rightID, encrypted)
				rightOptions := folderTestOptions(filepath.Join(root, "right-"+suffix), rightID, leftID, encrypted)
				leftOptions.ConflictCopies = true
				rightOptions.ConflictCopies = true
				left := createTestFolder(t, leftOptions, map[string][]byte{name: []byte("initial asset")})
				right := createTestFolder(t, rightOptions, nil)
				initial := exchangeFolderIndex(t, left, right, true, 0)
				if _, err := right.Step(ctx, func(protocol.DeviceID) RequestFunc { return snapshotRequester(t, left, initial, nil) }); err != nil {
					t.Fatal(err)
				}
				leftBytes, rightBytes := []byte("left offline edit"), []byte("right offline edit")
				if same {
					rightBytes = bytes.Clone(leftBytes)
				}
				for _, change := range []struct {
					folder *Folder
					data   []byte
				}{{left, leftBytes}, {right, rightBytes}} {
					if err := os.WriteFile(filepath.Join(change.folder.options.Directory, filepath.FromSlash(name)), change.data, 0600); err != nil {
						t.Fatal(err)
					}
					if err := change.folder.Invalidate(); err != nil {
						t.Fatal(err)
					}
					if _, err := change.folder.Refresh(ctx); err != nil {
						t.Fatal(err)
					}
				}
				a, b := folderSnapshot(t, left), folderSnapshot(t, right)
				want, losing := rightBytes, leftBytes
				if a.Files[0].Info.WinsConflict(b.Files[0].Info) {
					want, losing = leftBytes, rightBytes
				}
				// Both sides see the original concurrent versions, even when the
				// first node has already applied its local resolution.
				sendFrozenIndex(t, left, right, a)
				sendFrozenIndex(t, right, left, b)
				if _, err := left.Step(ctx, func(protocol.DeviceID) RequestFunc { return snapshotRequester(t, right, b, nil) }); err != nil {
					t.Fatal(err)
				}
				if _, err := right.Step(ctx, func(protocol.DeviceID) RequestFunc { return snapshotRequester(t, left, a, nil) }); err != nil {
					t.Fatal(err)
				}
				resolvedA, resolvedB := folderSnapshot(t, left), folderSnapshot(t, right)
				copies := map[string]bool{}
				for _, folder := range []*Folder{left, right} {
					data, err := os.ReadFile(filepath.Join(folder.options.Directory, filepath.FromSlash(name)))
					if err != nil || !bytes.Equal(data, want) {
						t.Fatal("different native conflict winner", err, string(data))
					}
					view := folderSnapshot(t, folder)
					if len(view.Files) != map[bool]int{false: 2, true: 1}[same] {
						t.Fatal("conflict files were duplicated or omitted", view.Files)
					}
					for _, entry := range view.Files {
						if entry.Info.Name != name {
							copies[entry.Info.Name] = true
							data, err := os.ReadFile(filepath.Join(folder.options.Directory, filepath.FromSlash(entry.Info.Name)))
							if err != nil || !bytes.Equal(data, losing) {
								t.Fatal("losing bytes were not materialized", err)
							}
						}
					}
				}
				if !same && len(copies) != 1 {
					t.Fatal("nodes selected different conflict-copy names", copies)
				}
				sendFrozenIndex(t, left, right, resolvedA)
				sendFrozenIndex(t, right, left, resolvedB)
				for _, folder := range []*Folder{left, right} {
					if _, err := folder.Step(ctx, nil); err != nil {
						t.Fatal("resolved indexes did not converge", err)
					}
					stable := folderSnapshot(t, folder)
					for i := 0; i < 3; i++ {
						if _, err := folder.Step(ctx, nil); err != nil {
							t.Fatal(err)
						}
					}
					if folderSnapshot(t, folder).Catalog != stable.Catalog {
						t.Fatal("resolved conflict kept generating history")
					}
				}
			})
		}
	}
}
