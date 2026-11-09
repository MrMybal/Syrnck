package syncthing

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/syncthing/syncthing/lib/protocol"
)

func TestFolderResumesInterruptedBlocksAfterReopen(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		for _, corrupt := range []bool{false, true} {
			t.Run(fmt.Sprintf("%v/corrupt=%v", encrypted, corrupt), func(t *testing.T) {
				ctx := context.Background()
				root := tempDirectory(t)
				leftID, rightID := folderProcessIDs()
				suffix := map[bool]string{false: "plain", true: "encrypted"}[encrypted]
				data := bytes.Join([][]byte{block(11), block(12), block(13)}, nil)
				left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left-"+suffix), leftID, rightID, encrypted), map[string][]byte{"asset": data})
				options := folderTestOptions(filepath.Join(root, "right-"+suffix), rightID, leftID, encrypted)
				options.ResumeDownloads = true
				right := createTestFolder(t, options, nil)
				snapshot := exchangeFolderIndex(t, left, right, true, 0)
				original := snapshotRequester(t, left, snapshot, nil)
				requests := 0
				interrupted := errors.New("test peer disconnected")
				_, err := right.Step(ctx, func(protocol.DeviceID) RequestFunc {
					return func(ctx context.Context, req *protocol.Request) ([]byte, error) {
						requests++
						if requests == 2 {
							return nil, interrupted
						}
						return original(ctx, req)
					}
				})
				if !errors.Is(err, interrupted) {
					t.Fatal(err)
				}
				if _, err := os.Stat(filepath.Join(options.Directory, "asset")); !errors.Is(err, os.ErrNotExist) {
					t.Fatal("partial file materialized", err)
				}
				plan := &folderDownload{Device: leftID, IndexID: snapshot.IndexID, Info: snapshot.Files[0].Info}
				resume, err := right.acquireResume(plan)
				if err != nil {
					t.Fatal("lease held after failure", err)
				}
				if _, err := right.acquireResume(plan); !errors.Is(err, ErrFolderDownloadBusy) {
					resume.Close()
					t.Fatal("concurrent controller obtained same staging", err)
				}
				stagePath := resume.directory
				resume.Close()
				if corrupt {
					file, err := os.OpenFile(filepath.Join(stagePath, "data"), os.O_WRONLY, 0)
					if err != nil {
						t.Fatal(err)
					}
					_, err = file.WriteAt([]byte("corrupt"), 0)
					err = errors.Join(err, file.Close())
					if err != nil {
						t.Fatal(err)
					}
				}
				if err := right.Close(); err != nil {
					t.Fatal(err)
				}
				reopened, err := NewFolder(ctx, options)
				if err != nil {
					t.Fatal(err)
				}
				defer reopened.Close()
				var offsets []int64
				status, err := reopened.Step(ctx, func(protocol.DeviceID) RequestFunc {
					return func(ctx context.Context, req *protocol.Request) ([]byte, error) {
						offsets = append(offsets, req.Offset)
						return original(ctx, req)
					}
				})
				if err != nil {
					t.Fatal(err)
				}
				wantRequests := 2
				if corrupt {
					wantRequests++
				}
				if len(offsets) != wantRequests || status.Transfer.Requests != uint64(wantRequests) {
					t.Fatal("resume transferred wrong ranges", offsets, status.Transfer)
				}
				if !corrupt && (offsets[0] == 0 || status.Transfer.StagedBytes != protocol.MinBlockSize) {
					t.Fatal("validated range downloaded again", offsets, status.Transfer)
				}
				physical, err := os.ReadFile(filepath.Join(options.Directory, "asset"))
				if err != nil || !bytes.Equal(physical, data) {
					t.Fatal("wrong resumed file", err)
				}
				if _, err := os.Stat(stagePath); !errors.Is(err, os.ErrNotExist) {
					t.Fatal("retained completed plaintext staging", err)
				}
				// A changed peer generation or index never selects the old private path.
				first, err := reopened.acquireResume(plan)
				if err != nil {
					t.Fatal(err)
				}
				oldPath := first.directory
				first.Close()
				plan.IndexID++
				second, err := reopened.acquireResume(plan)
				if err != nil {
					t.Fatal(err)
				}
				defer second.Close()
				if second.directory == oldPath {
					t.Fatal("peer generation reused staging identity")
				}
			})
		}
	}
}

func TestFolderResumeNeverDeletesForeignEntries(t *testing.T) {
	left, right := folderProcessIDs()
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), right, left, false), map[string][]byte{"asset": []byte("data")})
	snapshot := folderSnapshot(t, folder)
	resume, err := folder.acquireResume(&folderDownload{Device: left, IndexID: 1, Info: snapshot.Files[0].Info})
	if err != nil {
		t.Fatal(err)
	}
	defer resume.Close()
	if err := os.Mkdir(resume.directory, 0700); err != nil {
		t.Fatal(err)
	}
	foreign := filepath.Join(resume.directory, "keep")
	if err := os.WriteFile(foreign, []byte("unrelated"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := resume.retire(); err == nil {
		t.Fatal("foreign staging retired")
	}
	if data, err := os.ReadFile(foreign); err != nil || string(data) != "unrelated" {
		t.Fatal("foreign bytes removed", err)
	}
}

func TestFolderResumeRequiresMatchingPrivateIdentity(t *testing.T) {
	ctx := context.Background()
	if folder, err := NewFolder(ctx, FolderOptions{ResumeDownloads: true}); err == nil || folder != nil {
		t.Fatal("resume accepted an implicit Store path", err)
	}
	left, right := folderProcessIDs()
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), right, left, false), map[string][]byte{"asset": []byte("data")})
	info := folderSnapshot(t, folder).Files[0].Info
	resume, err := folder.acquireResume(&folderDownload{Device: left, IndexID: 1, Info: info})
	if err != nil {
		t.Fatal(err)
	}
	defer resume.Close()
	file, cleanup, err := stage("project", info, resume.directory)
	if err != nil {
		t.Fatal(err)
	}
	defer cleanup()
	if _, err := file.Write([]byte("data")); err != nil {
		file.Close()
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	statePath := filepath.Join(resume.directory, "state.json")
	if err := os.WriteFile(statePath, []byte("changed"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := resume.retire(); err == nil {
		t.Fatal("changed staging identity retired")
	}
	if data, err := os.ReadFile(filepath.Join(resume.directory, "data")); err != nil || string(data) != "data" {
		t.Fatal("identity mismatch removed data", err)
	}
	if err := os.WriteFile(statePath, resume.metadata, 0600); err != nil {
		t.Fatal(err)
	}
	if err := resume.retire(); err != nil {
		t.Fatal("matching staging could not be retired", err)
	}
}

func TestFolderResumeRejectsLinkedStagingRoot(t *testing.T) {
	left, right := folderProcessIDs()
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), right, left, false), nil)
	outside := tempDirectory(t)
	if err := os.Symlink(outside, filepath.Join(folder.options.Storage, "bep-downloads")); err != nil {
		t.Skip("local symlink creation unavailable", err)
	}
	if lease, err := folder.acquireResume(&folderDownload{}); err == nil || lease != nil {
		if lease != nil {
			lease.Close()
		}
		t.Fatal("linked staging root accepted", err)
	}
	entries, err := os.ReadDir(outside)
	if err != nil || len(entries) != 0 {
		t.Fatal("linked target was modified", err)
	}
}
