package syncthing

import (
	"bytes"
	"context"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func TestStandaloneServiceWithReferenceDaemonAndOfflineRestart(t *testing.T) {
	executable, referenceBinary := os.Getenv("SYRNCK_TEST_BEP_SERVICE"), os.Getenv("SYRNCK_SYNCTHING_BINARY")
	if executable == "" || referenceBinary == "" {
		t.Skip("build BEP service and reference daemon for standalone interoperability")
	}
	if !filepath.IsAbs(executable) || !filepath.IsAbs(referenceBinary) {
		t.Fatal("qualification executables must be absolute")
	}
	for _, encrypted := range []bool{false, true} {
		t.Run(map[bool]string{false: "plain", true: "encrypted"}[encrypted], func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			certificate := deviceCertificate(t)
			local := protocol.NewDeviceID(certificate.Certificate[0])
			name, renamed := "Content/Map.uasset", "Content/Renamed.uasset"
			before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
			after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
			reference := startReference(t, ctx, referenceBinary, local, name, before)
			root := tempDirectory(t)
			directory := filepath.Join(root, "work")
			storage := filepath.Join(directory, ".syrnck")
			if err := os.Mkdir(directory, 0700); err != nil {
				t.Fatal(err)
			}
			var key []byte
			if encrypted {
				key = bytes.Repeat([]byte{92}, 32)
			}
			t.Setenv("SYRNCK_BEP_DAEMON_TEST_KEY", hex.EncodeToString(key))
			store, err := syrnck.Open(storage, syrnck.Options{Create: true, Key: key})
			if err != nil {
				t.Fatal(err)
			}
			store.Close()
			certPath, keyPath := filepath.Join(root, "cert.pem"), filepath.Join(root, "key.pem")
			private, err := x509.MarshalPKCS8PrivateKey(certificate.PrivateKey)
			if err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(certPath, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certificate.Certificate[0]}), 0600); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: private}), 0600); err != nil {
				t.Fatal(err)
			}
			configuration := map[string]any{"version": 1, "directory": directory, "storage": storage, "folder": "project", "certificate": certPath, "private_key": keyPath,
				"peer": reference.device.String(), "mode": "dial", "address": reference.bep, "key_env": "SYRNCK_BEP_DAEMON_TEST_KEY", "poll_ms": 100, "retry_min_ms": 50, "retry_max_ms": 200}
			encoded, err := json.Marshal(configuration)
			if err != nil {
				t.Fatal(err)
			}
			configPath := filepath.Join(root, "service.json")
			if err := os.WriteFile(configPath, encoded, 0600); err != nil {
				t.Fatal(err)
			}
			launch := func(label string) func() {
				output, err := os.Create(filepath.Join(root, label+"-events.log"))
				if err != nil {
					t.Fatal(err)
				}
				diagnostics, err := os.Create(filepath.Join(root, label+"-errors.log"))
				if err != nil {
					output.Close()
					t.Fatal(err)
				}
				command := exec.CommandContext(ctx, executable, "run", "--config", configPath)
				hideReferenceProcess(command)
				command.Stdout = output
				command.Stderr = diagnostics
				command.Env = os.Environ()
				if err := command.Start(); err != nil {
					output.Close()
					diagnostics.Close()
					t.Fatal(err)
				}
				var stopped sync.Once
				stop := func() {
					stopped.Do(func() { _ = command.Process.Kill(); _ = command.Wait(); _ = output.Close(); _ = diagnostics.Close() })
				}
				t.Cleanup(func() {
					stop()
					if t.Failed() {
						for _, suffix := range []string{"-events.log", "-errors.log"} {
							data, _ := os.ReadFile(filepath.Join(root, label+suffix))
							t.Log(label, suffix, string(data))
						}
					}
				})
				return stop
			}
			waitApplied := func(name string, wanted []byte) {
				t.Helper()
				for {
					physical, err := os.ReadFile(filepath.Join(directory, filepath.FromSlash(name)))
					ack := false
					if err == nil && bytes.Equal(physical, wanted) {
						store, openErr := syrnck.Open(storage, syrnck.Options{Key: key})
						if openErr == nil {
							catalog, readErr := OpenCatalog(ctx, store, CatalogOptions{Folder: "project", LocalDevice: local, Peers: []protocol.DeviceID{reference.device}})
							if readErr == nil {
								for _, entry := range catalog.state.Files {
									if entry.Info.Name == name && !entry.Info.Deleted && entry.Manifest != "" {
										data, readErr := store.ReadRange(entry.Manifest, 0, uint64(len(wanted)))
										ack = readErr == nil && bytes.Equal(data, wanted)
									}
								}
							}
							store.Close()
						}
					}
					if ack {
						return
					}
					select {
					case <-ctx.Done():
						t.Fatal("standalone service did not acknowledge native application", name, ctx.Err(), err)
					case <-time.After(25 * time.Millisecond):
					}
				}
			}
			stop := launch("first")
			waitApplied(name, before)
			if err := os.WriteFile(filepath.Join(directory, filepath.FromSlash(name)), after, 0600); err != nil {
				t.Fatal(err)
			}
			reference.waitFile(t, ctx, name, after)
			stop()
			if err := os.Rename(filepath.Join(reference.folder, filepath.FromSlash(name)), filepath.Join(reference.folder, filepath.FromSlash(renamed))); err != nil {
				t.Fatal(err)
			}
			reference.scan(t, ctx)
			stop = launch("restarted")
			waitApplied(renamed, after)
			for {
				_, err := os.Stat(filepath.Join(directory, filepath.FromSlash(name)))
				if os.IsNotExist(err) {
					break
				}
				select {
				case <-ctx.Done():
					t.Fatal("offline upstream rename did not remove old native path", err)
				case <-time.After(25 * time.Millisecond):
				}
			}
			stop()
		})
	}
}
