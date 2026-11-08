package syncthing

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	syrnck "syrnck.local/bindings/go"
)

func catalogProcessKey(encrypted bool) []byte {
	if encrypted {
		return bytes.Repeat([]byte{19}, 32)
	}
	return nil
}

func TestCatalogProcessChild(t *testing.T) {
	root := os.Getenv("SYRNCK_BEP_CATALOG_CHILD")
	if root == "" {
		t.Skip("private catalog termination helper")
	}
	encrypted := os.Getenv("SYRNCK_BEP_CATALOG_ENCRYPTED") == "true"
	store, err := syrnck.Open(root, syrnck.Options{Key: catalogProcessKey(encrypted)})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	catalog := mustCatalog(t, store, catalogOptions())
	entry := preparedEntry(t, store, "asset", []byte("complete second version"))
	if err := catalog.RecordLocal(entry); err != nil {
		t.Fatal(err)
	}
	if os.Getenv("SYRNCK_BEP_CATALOG_PHASE") == "after" {
		mustCommitCatalog(t, store, catalog)
	}
	message, _ := json.Marshal(map[string]string{"checkpoint": "catalog-ready", "manifest": entry.Manifest})
	fmt.Println(string(message))
	select {} // The parent kills this process with the native Store still open.
}

func TestCatalogSurvivesProcessTerminationBeforeAndAfterPublication(t *testing.T) {
	cli := os.Getenv("SYRNCK_TEST_CLI")
	if cli == "" {
		t.Fatal("catalog termination qualification requires built CLI")
	}
	for _, encrypted := range []bool{false, true} {
		for _, phase := range []string{"before", "after"} {
			t.Run(fmt.Sprintf("%v-%s", encrypted, phase), func(t *testing.T) {
				root := filepath.Join(tempDirectory(t), "store")
				key := catalogProcessKey(encrypted)
				store, err := syrnck.Open(root, syrnck.Options{Create: true, Key: key})
				if err != nil {
					t.Fatal(err)
				}
				catalog := mustCatalog(t, store, catalogOptions())
				original := preparedEntry(t, store, "asset", []byte("complete first version"))
				if err := catalog.RecordLocal(original); err != nil {
					t.Fatal(err)
				}
				mustCommitCatalog(t, store, catalog)
				oldRevision := catalog.Revision()
				if err := store.Close(); err != nil {
					t.Fatal(err)
				}
				command := exec.Command(os.Args[0], "-test.run=^TestCatalogProcessChild$", "-test.timeout=20s")
				command.Env = append(os.Environ(), "SYRNCK_BEP_CATALOG_CHILD="+root, "SYRNCK_BEP_CATALOG_ENCRYPTED="+fmt.Sprint(encrypted), "SYRNCK_BEP_CATALOG_PHASE="+phase)
				var diagnostics bytes.Buffer
				command.Stderr = &diagnostics
				stdout, err := command.StdoutPipe()
				if err != nil {
					t.Fatal(err)
				}
				if err := command.Start(); err != nil {
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
				var checkpoint map[string]string
				select {
				case line := <-ready:
					if err := json.Unmarshal([]byte(line), &checkpoint); err != nil {
						_ = command.Process.Kill()
						_ = command.Wait()
						t.Fatalf("catalog checkpoint %q: %v %s", line, err, diagnostics.String())
					}
				case <-time.After(8 * time.Second):
					_ = command.Process.Kill()
					_ = command.Wait()
					t.Fatal("catalog termination checkpoint timed out", diagnostics.String())
				}
				if checkpoint["checkpoint"] != "catalog-ready" {
					t.Fatal("wrong checkpoint")
				}
				if err := command.Process.Kill(); err != nil {
					t.Fatal(err)
				}
				if err := command.Wait(); err == nil {
					t.Fatal("child was not terminated")
				}
				command = exec.Command(cli, "--store", root, "gc", "--delete")
				for _, entry := range os.Environ() {
					if !strings.HasPrefix(strings.ToUpper(entry), "SYRNCK_KEY=") {
						command.Env = append(command.Env, entry)
					}
				}
				if encrypted {
					command.Env = append(command.Env, "SYRNCK_KEY="+hex.EncodeToString(key))
				}
				if output, err := command.CombinedOutput(); err != nil {
					t.Fatalf("restart GC: %v %s", err, output)
				}
				store, err = syrnck.Open(root, syrnck.Options{Key: key})
				if err != nil {
					t.Fatal(err)
				}
				defer store.Close()
				catalog = mustCatalog(t, store, catalogOptions())
				entry := localEntry(t, catalog, "asset")
				expectedManifest := original.Manifest
				expectedData := []byte("complete first version")
				if phase == "after" {
					expectedManifest = checkpoint["manifest"]
					expectedData = []byte("complete second version")
				}
				if entry.Manifest != expectedManifest {
					t.Fatalf("partial catalog published after kill: %s", phase)
				}
				actual, err := store.ReadRange(entry.Manifest, 0, uint64(len(expectedData)))
				if err != nil || !bytes.Equal(actual, expectedData) {
					t.Fatal("restart reconstructed wrong bytes", err)
				}
				if _, err := store.Get(oldRevision); err != nil {
					t.Fatal("historical catalog lost", err)
				}
				if err := store.VerifyFile(original.Manifest); err != nil {
					t.Fatal("original data lost", err)
				}
				if _, err := store.Verify(); err != nil {
					t.Fatal(err)
				}
			})
		}
	}
}
