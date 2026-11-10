package syncthing

import (
	"bytes"
	"context"
	"crypto/x509"
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
)

func launchOpaqueService(t *testing.T, ctx context.Context, executable, config, root, label string) (string, func()) {
	t.Helper()
	outPath, errPath := filepath.Join(root, label+"-events.log"), filepath.Join(root, label+"-errors.log")
	output, err := os.Create(outPath)
	if err != nil {
		t.Fatal(err)
	}
	diagnostics, err := os.Create(errPath)
	if err != nil {
		output.Close()
		t.Fatal(err)
	}
	command := exec.CommandContext(ctx, executable, "--lang", "fr", "run", "--config", config)
	command.Env = referenceEnvironment() // No content password or native key is inherited.
	command.Stdout, command.Stderr = output, diagnostics
	hideReferenceProcess(command)
	if err := command.Start(); err != nil {
		output.Close()
		diagnostics.Close()
		t.Fatal(err)
	}
	exited := make(chan struct{})
	go func() { command.Wait(); close(exited) }()
	var once sync.Once
	stop := func() { once.Do(func() { command.Process.Kill(); <-exited; output.Close(); diagnostics.Close() }) }
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
		var ready struct {
			Event            string `json:"event"`
			Address          string `json:"address"`
			Encrypted        bool   `json:"encrypted"`
			ReceiveEncrypted bool   `json:"receive_encrypted"`
		}
		if json.NewDecoder(bytes.NewReader(data)).Decode(&ready) == nil && ready.Event == "ready" {
			if !ready.Encrypted || !ready.ReceiveEncrypted {
				t.Fatal("standalone did not acknowledge opaque reception")
			}
			return ready.Address, stop
		}
		select {
		case <-exited:
			t.Fatal("opaque service exited before ready")
		case <-ctx.Done():
			t.Fatal("opaque service startup timed out", ctx.Err())
		case <-time.After(10 * time.Millisecond):
		}
	}
}

func TestEncryptedReceiverStandaloneRestartsAndRestores(t *testing.T) {
	executable := os.Getenv("SYRNCK_TEST_BEP_SERVICE")
	if executable == "" {
		t.Skip("build standalone BEP receiver")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	serverCert, sourceCert := deviceCertificate(t), deviceCertificate(t)
	serverID, sourceID := protocol.NewDeviceID(serverCert.Certificate[0]), protocol.NewDeviceID(sourceCert.Certificate[0])
	receiver := sessionTestReceiver(t, ctx, serverCert, []protocol.DeviceID{sourceID})
	root := tempDirectory(t)
	private, err := x509.MarshalPKCS8PrivateKey(serverCert.PrivateKey)
	if err != nil {
		t.Fatal(err)
	}
	certPath, keyPath := filepath.Join(root, "cert.pem"), filepath.Join(root, "key.pem")
	for path, data := range map[string][]byte{certPath: pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: serverCert.Certificate[0]}), keyPath: pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: private})} {
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	config := map[string]any{"version": 1, "receive_encrypted": true, "storage": receiver.options.Storage, "folder": "project",
		"certificate": certPath, "private_key": keyPath, "peer": sourceID.String(), "mode": "listen", "address": "127.0.0.1:0",
		"poll_ms": 50, "retry_min_ms": 50, "retry_max_ms": 200}
	encoded, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(root, "receiver.json")
	if err := os.WriteFile(configPath, encoded, 0600); err != nil {
		t.Fatal(err)
	}
	const password = "test-client-only-opaque-content-password"
	name := "Content/Map.uasset"
	body := bytes.Join([][]byte{block(1), block(2), []byte("tail")}, nil)
	source := createTestFolder(t, folderTestOptions(filepath.Join(root, "source-plain"), sourceID, serverID, false), map[string][]byte{name: body})
	connect := func(address string, folder *Folder) *FolderSession {
		t.Helper()
		stream, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", address)
		if err != nil {
			t.Fatal(err)
		}
		session, err := OpenFolderSession(ctx, folder, stream, FolderSessionOptions{Device: DeviceOptions{Certificate: sourceCert, RemoteDevice: serverID}, Untrusted: true, EncryptionPassword: password, PollInterval: 50 * time.Millisecond})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { session.Close() })
		return session
	}
	address, stop := launchOpaqueService(t, ctx, executable, configPath, root, "first")
	client := connect(address, source)
	folderKey := protocol.NewKeyGenerator().KeyFromPassword("project", password)
	// Do not contend with the child's exclusive native Store just to probe
	// progress. Wait for its durable complete-file report, stop, then inspect.
	for {
		data, _ := os.ReadFile(filepath.Join(root, "first-events.log"))
		received := false
		for _, line := range bytes.Split(data, []byte{'\n'}) {
			var frame struct {
				Event  string       `json:"event"`
				Status FolderStatus `json:"status"`
			}
			if json.Unmarshal(line, &frame) == nil && frame.Event == "folder" && frame.Status.State == "received" && frame.Status.Catalog != "" {
				received = true
				break
			}
		}
		if received {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatal("standalone opaque reception timed out", ctx.Err())
		case <-time.After(25 * time.Millisecond):
		}
	}
	client.Close()
	stop() // Actual process termination, not the graceful library Close path.
	waitOpaqueEntry(t, ctx, receiver, folderKey, name, body, client)
	opaqueTestGC(t, receiver, false)
	address, stop = launchOpaqueService(t, ctx, executable, configPath, root, "restart")
	// Reprovision the same explicitly paired identity with an empty local Store.
	restore := createTestFolder(t, folderTestOptions(filepath.Join(root, "restored-encrypted"), sourceID, serverID, true), nil)
	restored := connect(address, restore)
	waitFolderApplication(t, ctx, restore, name, body, restored)
	restored.Close()
	stop()
}
