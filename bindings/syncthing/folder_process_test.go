package syncthing

import (
	"bufio"
	"bytes"
	"context"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func folderProcessIDs() (protocol.DeviceID, protocol.DeviceID) {
	return protocol.NewDeviceID([]byte("folder kill source")), protocol.NewDeviceID([]byte("folder kill destination"))
}
func TestFolderProcessChild(t *testing.T) {
	root := os.Getenv("SYRNCK_FOLDER_CHILD")
	if root == "" {
		t.Skip("private folder termination helper")
	}
	left, right := folderProcessIDs()
	options := folderTestOptions(root, right, left, strings.Contains(filepath.Base(root), "encrypted"))
	folder, err := NewFolder(context.Background(), options)
	if err != nil {
		t.Fatal(err)
	}
	defer folder.Close()
	err = folder.withStore(context.Background(), func(store *syrnck.Store) error {
		loaded, err := folder.load(context.Background(), store)
		if err != nil {
			return err
		}
		if loaded.pending == nil {
			t.Fatal("child missing durable application")
		}
		if os.Getenv("SYRNCK_FOLDER_PHASE") == "after" {
			p := loaded.pending
			if _, err = folder.workspace.CheckoutOwnedContext(context.Background(), store, p.Expected.native(), p.Candidate, p.Owner); err != nil {
				return err
			}
		}
		fmt.Println("folder-checkpoint")
		select {} // Parent terminates the process while its native Store is open.
	})
	if err != nil {
		t.Fatal(err)
	}
}

func TestFolderRecoveryAfterProcessTermination(t *testing.T) {
	ctx := context.Background()
	cli := os.Getenv("SYRNCK_TEST_CLI")
	if cli == "" {
		t.Fatal("requires built CLI")
	}
	for _, encrypted := range []bool{false, true} {
		for _, phase := range []string{"before", "after"} {
			t.Run(fmt.Sprintf("%v-%s", encrypted, phase), func(t *testing.T) {
				root := tempDirectory(t)
				leftID, rightID := folderProcessIDs()
				suffix := "plain"
				var key []byte
				if encrypted {
					suffix = "encrypted"
					key = bytes.Repeat([]byte{83}, 32)
				}
				left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left-"+suffix), leftID, rightID, encrypted), map[string][]byte{"asset": []byte("complete incoming bytes")})
				options := folderTestOptions(filepath.Join(root, "right-"+suffix), rightID, leftID, encrypted)
				right := createTestFolder(t, options, nil)
				prepareFolderApplication(t, left, right, "asset")
				source := folderSnapshot(t, left).Files[0]
				if err := right.Close(); err != nil {
					t.Fatal(err)
				}
				command := exec.Command(os.Args[0], "-test.run=^TestFolderProcessChild$", "-test.timeout=20s")
				command.Env = append(os.Environ(), "SYRNCK_FOLDER_CHILD="+options.Directory, "SYRNCK_FOLDER_PHASE="+phase)
				var diagnostics bytes.Buffer
				command.Stderr = &diagnostics
				stdout, err := command.StdoutPipe()
				if err != nil {
					t.Fatal(err)
				}
				if err = command.Start(); err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() {
					if command.ProcessState == nil {
						_ = command.Process.Kill()
						_ = command.Wait()
					}
				})
				ready := make(chan string, 1)
				go func() { line, _ := bufio.NewReader(stdout).ReadString('\n'); ready <- line }()
				select {
				case line := <-ready:
					if line != "folder-checkpoint\n" {
						_ = command.Process.Kill()
						_ = command.Wait()
						t.Fatal("wrong child checkpoint", line, diagnostics.String())
					}
				case <-time.After(8 * time.Second):
					_ = command.Process.Kill()
					_ = command.Wait()
					t.Fatal("folder checkpoint timed out", diagnostics.String())
				}
				if err = command.Process.Kill(); err != nil {
					t.Fatal(err)
				}
				if err = command.Wait(); err == nil {
					t.Fatal("child was not terminated")
				}
				gc := exec.Command(cli, "--store", options.Storage, "gc", "--delete")
				for _, value := range os.Environ() {
					if !strings.HasPrefix(strings.ToUpper(value), "SYRNCK_KEY=") {
						gc.Env = append(gc.Env, value)
					}
				}
				if encrypted {
					gc.Env = append(gc.Env, "SYRNCK_KEY="+hex.EncodeToString(key))
				}
				if output, err := gc.CombinedOutput(); err != nil {
					t.Fatalf("GC with pending application: %v %s", err, output)
				}
				wanted := []byte("complete incoming bytes")
				if phase == "after" {
					wanted = []byte("local edit after native publication")
					if err = os.WriteFile(filepath.Join(options.Directory, "asset"), wanted, 0600); err != nil {
						t.Fatal(err)
					}
				}
				reopened, err := NewFolder(ctx, options)
				if err != nil {
					t.Fatal(err)
				}
				defer reopened.Close()
				if _, err = reopened.Refresh(ctx); err != nil {
					t.Fatal("owned application recovery", err)
				}
				snapshot := folderSnapshot(t, reopened)
				if len(snapshot.Files) != 1 {
					t.Fatal("unexpected recovered files", snapshot.Files)
				}
				entry := snapshot.Files[0]
				if phase == "after" && entry.Info.Version.Compare(source.Info.Version) != protocol.Greater {
					t.Fatal("post-publication edit lost its causal base")
				}
				if phase == "before" && !entry.Info.Version.Equal(source.Info.Version) {
					t.Fatal("recovery invented a local edit")
				}
				actual, err := os.ReadFile(filepath.Join(options.Directory, "asset"))
				if err != nil || !bytes.Equal(actual, wanted) {
					t.Fatal("wrong recovered working bytes", err)
				}
				err = reopened.withStore(ctx, func(store *syrnck.Store) error {
					data, err := store.ReadRange(entry.Manifest, 0, uint64(len(wanted)))
					if err != nil {
						return err
					}
					if !bytes.Equal(data, wanted) {
						t.Fatal("wrong recovered immutable data")
					}
					if err = store.VerifyFile(source.Manifest); err != nil {
						return err
					}
					refs, err := store.Refs()
					if err != nil {
						return err
					}
					for name := range refs {
						if strings.Contains(name, "/pending/") {
							t.Fatal("completed application kept pending roots", name)
						}
					}
					_, err = store.Verify()
					return err
				})
				if err != nil {
					t.Fatal(err)
				}
			})
		}
	}
}
