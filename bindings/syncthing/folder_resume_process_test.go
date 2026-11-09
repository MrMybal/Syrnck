package syncthing

import (
	"bytes"
	"context"
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

func TestFolderResumeProcessChild(t *testing.T) {
	root := os.Getenv("SYRNCK_FOLDER_RESUME_CHILD")
	if root == "" {
		t.Skip("private persistent folder download helper")
	}
	encrypted := strings.Contains(filepath.Base(root), "encrypted")
	leftID, rightID := folderProcessIDs()
	left, err := NewFolder(context.Background(), folderTestOptions(filepath.Join(root, "left"), leftID, rightID, encrypted))
	if err != nil {
		t.Fatal(err)
	}
	defer left.Close()
	options := folderTestOptions(filepath.Join(root, "right"), rightID, leftID, encrypted)
	options.ResumeDownloads = true
	right, err := NewFolder(context.Background(), options)
	if err != nil {
		t.Fatal(err)
	}
	defer right.Close()
	snapshot := folderSnapshot(t, left)
	request := snapshotRequester(t, left, snapshot, nil)
	requests := 0
	_, err = right.Step(context.Background(), func(protocol.DeviceID) RequestFunc {
		return func(ctx context.Context, req *protocol.Request) ([]byte, error) {
			requests++
			if requests == 2 {
				fmt.Println("folder-resume-checkpoint")
				select {}
			}
			return request(ctx, req)
		}
	})
	t.Fatal("download checkpoint missed", err)
}

func TestFolderDownloadResumeAfterHardTermination(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			root := filepath.Join(tempDirectory(t), map[bool]string{false: "plain", true: "encrypted"}[encrypted])
			leftID, rightID := folderProcessIDs()
			// createTestFolder recognizes encryption in the directory basename.
			// Explicit creation here keeps left/right paths stable in the child.
			leftOptions := folderTestOptions(filepath.Join(root, "left"), leftID, rightID, encrypted)
			rightOptions := folderTestOptions(filepath.Join(root, "right"), rightID, leftID, encrypted)
			data := bytes.Join([][]byte{block(21), block(22), block(23)}, nil)
			for _, options := range []FolderOptions{leftOptions, rightOptions} {
				if err := os.MkdirAll(options.Directory, 0700); err != nil {
					t.Fatal(err)
				}
			}
			if err := os.WriteFile(filepath.Join(leftOptions.Directory, "asset"), data, 0600); err != nil {
				t.Fatal(err)
			}
			var key []byte
			if encrypted {
				key = bytes.Repeat([]byte{83}, 32)
			}
			for _, options := range []FolderOptions{leftOptions, rightOptions} {
				store, err := syrnck.Open(options.Storage, syrnck.Options{Create: true, Key: key})
				if err != nil {
					t.Fatal(err)
				}
				if err := store.Close(); err != nil {
					t.Fatal(err)
				}
			}
			left, err := NewFolder(context.Background(), leftOptions)
			if err != nil {
				t.Fatal(err)
			}
			right, err := NewFolder(context.Background(), rightOptions)
			if err != nil {
				left.Close()
				t.Fatal(err)
			}
			announced := exchangeFolderIndex(t, left, right, true, 0)
			if err := left.Close(); err != nil {
				t.Fatal(err)
			}
			if err := right.Close(); err != nil {
				t.Fatal(err)
			}
			command := exec.Command(os.Args[0], "-test.run=^TestFolderResumeProcessChild$", "-test.timeout=20s")
			command.Env = append(os.Environ(), "SYRNCK_FOLDER_RESUME_CHILD="+root)
			terminateProcessAtCheckpoint(t, command, "folder-resume-checkpoint\n", func() {
				probe, err := NewFolder(context.Background(), rightOptions)
				if err != nil {
					t.Fatal("download held native Store during network wait", err)
				}
				defer probe.Close()
				lease, err := probe.acquireResume(&folderDownload{Device: leftID, IndexID: announced.IndexID, Info: announced.Files[0].Info})
				if lease != nil {
					lease.Close()
				}
				if !errors.Is(err, ErrFolderDownloadBusy) {
					t.Fatal("another process obtained live download lease", err)
				}
			})
			left, err = NewFolder(context.Background(), leftOptions)
			if err != nil {
				t.Fatal(err)
			}
			defer left.Close()
			rightOptions.ResumeDownloads = true
			right, err = NewFolder(context.Background(), rightOptions)
			if err != nil {
				t.Fatal(err)
			}
			defer right.Close()
			snapshot := folderSnapshot(t, left)
			request := snapshotRequester(t, left, snapshot, nil)
			var offsets []int64
			status, err := right.Step(context.Background(), func(protocol.DeviceID) RequestFunc {
				return func(ctx context.Context, req *protocol.Request) ([]byte, error) {
					offsets = append(offsets, req.Offset)
					return request(ctx, req)
				}
			})
			if err != nil {
				t.Fatal("resume lease or ranges did not survive termination", err)
			}
			if len(offsets) != 2 || offsets[0] == 0 || status.Transfer.StagedBytes != protocol.MinBlockSize {
				t.Fatal("hard termination lost verified ranges", offsets, status.Transfer)
			}
			physical, err := os.ReadFile(filepath.Join(rightOptions.Directory, "asset"))
			if err != nil || !bytes.Equal(physical, data) {
				t.Fatal("wrong resumed asset", err)
			}
		})
	}
}
