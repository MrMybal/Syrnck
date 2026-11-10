package syncthing

import (
	"bytes"
	"context"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func TestReferenceUntrustedStandaloneRestart(t *testing.T) {
	binary, executable := os.Getenv("SYRNCK_SYNCTHING_BINARY"), os.Getenv("SYRNCK_TEST_BEP_SERVICE")
	if binary == "" || executable == "" {
		t.Skip("build the reference daemon and standalone BEP service")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 35*time.Second)
	defer cancel()
	root := tempDirectory(t)
	certificate, recipient := deviceCertificate(t), deviceCertificate(t)
	local, remote := protocol.NewDeviceID(certificate.Certificate[0]), protocol.NewDeviceID(recipient.Certificate[0])
	reference := startReferencePeers(t, ctx, binary, []protocol.DeviceID{local, remote}, "", nil, true)
	directory, storage := filepath.Join(root, "source"), filepath.Join(root, "source/.syrnck")
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	storageKey := bytes.Repeat([]byte{42}, 32)
	store, err := syrnck.Open(storage, syrnck.Options{Create: true, Key: storageKey})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	name := "Private.bin"
	before, after := block(1), block(2)
	if err := os.WriteFile(filepath.Join(directory, name), before, 0600); err != nil {
		t.Fatal(err)
	}
	private, err := x509.MarshalPKCS8PrivateKey(certificate.PrivateKey)
	if err != nil {
		t.Fatal(err)
	}
	certPath, keyPath := filepath.Join(root, "cert.pem"), filepath.Join(root, "device.pem")
	for path, data := range map[string][]byte{certPath: pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certificate.Certificate[0]}),
		keyPath: pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: private})} {
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	const password = "test-standalone-untrusted-password 密钥"
	t.Setenv("SYRNCK_UNTRUSTED_SERVICE_KEY", hex.EncodeToString(storageKey))
	t.Setenv("SYRNCK_UNTRUSTED_SERVICE_PASSWORD", password)
	config := map[string]any{"version": 1, "directory": directory, "storage": storage, "folder": "project",
		"certificate": certPath, "private_key": keyPath, "peer": reference.device.String(), "mode": "dial", "address": reference.bep,
		"key_env": "SYRNCK_UNTRUSTED_SERVICE_KEY", "untrusted_peer": true, "folder_password_env": "SYRNCK_UNTRUSTED_SERVICE_PASSWORD",
		"poll_ms": 100, "retry_min_ms": 50, "retry_max_ms": 200}
	encoded, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(root, "service.json")
	if err := os.WriteFile(configPath, encoded, 0600); err != nil {
		t.Fatal(err)
	}
	launch := func(label string) func() {
		t.Helper()
		outPath, errPath := filepath.Join(root, label+"-out.log"), filepath.Join(root, label+"-err.log")
		output, err := os.Create(outPath)
		if err != nil {
			t.Fatal(err)
		}
		diagnostics, err := os.Create(errPath)
		if err != nil {
			output.Close()
			t.Fatal(err)
		}
		command := exec.CommandContext(ctx, executable, "--lang", "fr", "run", "--config", configPath)
		hideReferenceProcess(command)
		command.Env = os.Environ()
		command.Stdout = output
		command.Stderr = diagnostics
		if err := command.Start(); err != nil {
			output.Close()
			diagnostics.Close()
			t.Fatal(err)
		}
		var once sync.Once
		stop := func() {
			once.Do(func() { command.Process.Kill(); command.Wait(); output.Close(); diagnostics.Close() })
		}
		t.Cleanup(func() {
			stop()
			if t.Failed() {
				for _, path := range []string{outPath, errPath} {
					data, _ := os.ReadFile(path)
					t.Log(string(data))
				}
			}
		})
		for {
			data, _ := os.ReadFile(outPath)
			var frame struct {
				Event     string `json:"event"`
				Encrypted bool   `json:"encrypted"`
			}
			if json.NewDecoder(bytes.NewReader(data)).Decode(&frame) == nil && frame.Event == "ready" {
				if !frame.Encrypted {
					t.Fatal("standalone did not acknowledge encrypted mode")
				}
				return stop
			}
			select {
			case <-ctx.Done():
				t.Fatal("encrypted standalone did not initialize", ctx.Err())
			case <-time.After(10 * time.Millisecond):
			}
		}
	}
	stop := launch("first")
	second := createTestFolder(t, folderTestOptions(filepath.Join(root, "recipient"), remote, reference.device, false), nil)
	stream, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", reference.bep)
	if err != nil {
		t.Fatal(err)
	}
	session, err := OpenFolderSession(ctx, second, stream, FolderSessionOptions{Device: DeviceOptions{Certificate: recipient, RemoteDevice: reference.device},
		Untrusted: true, EncryptionPassword: password, PollInterval: 100 * time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { session.Close() })
	waitFolderApplication(t, ctx, second, name, before, session)
	stop()
	if err := os.WriteFile(filepath.Join(directory, name), after, 0600); err != nil {
		t.Fatal(err)
	}
	stop = launch("restart")
	waitFolderApplication(t, ctx, second, name, after, session)
	stop()
	for _, label := range []string{"first", "restart"} {
		data, _ := os.ReadFile(filepath.Join(root, label+"-out.log"))
		if bytes.Contains(data, []byte(password)) {
			t.Fatal("folder password leaked in service output")
		}
	}
	store, err = syrnck.Open(storage, syrnck.Options{Key: storageKey})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	catalog, err := OpenCatalog(ctx, store, CatalogOptions{Folder: "project", LocalDevice: local, Peers: []protocol.DeviceID{reference.device}})
	if err != nil {
		t.Fatal(err)
	}
	if len(catalog.state.Files) != 1 || catalog.state.Files[0].Info.Name != name {
		t.Fatal("standalone lost its durable catalog")
	}
	data, err := store.ReadRange(catalog.state.Files[0].Manifest, 0, uint64(len(after)))
	if err != nil || !bytes.Equal(data, after) {
		t.Fatal("offline source edit was not retained", err)
	}
}
