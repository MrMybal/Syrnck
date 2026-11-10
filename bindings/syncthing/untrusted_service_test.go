package syncthing

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

func TestReferenceUntrustedConcurrentServicesReconnect(t *testing.T) {
	binary := os.Getenv("SYRNCK_SYNCTHING_BINARY")
	if binary == "" {
		t.Skip("set SYRNCK_SYNCTHING_BINARY for concurrent encrypted services")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	certificates := []DeviceOptions{{Certificate: deviceCertificate(t)}, {Certificate: deviceCertificate(t)}}
	ids := []protocol.DeviceID{protocol.NewDeviceID(certificates[0].Certificate.Certificate[0]), protocol.NewDeviceID(certificates[1].Certificate.Certificate[0])}
	reference := startReferencePeers(t, ctx, binary, ids, "", nil, true)
	root := tempDirectory(t)
	first := createTestFolder(t, folderTestOptions(filepath.Join(root, "source"), ids[0], reference.device, false), map[string][]byte{"tiny.txt": []byte("private first")})
	second := createTestFolder(t, folderTestOptions(filepath.Join(root, "destination-encrypted"), ids[1], reference.device, true), nil)
	folders := []*Folder{first, second}
	prepared := make([]FolderSessionOptions, 2)
	for i := range prepared {
		certificates[i].RemoteDevice = reference.device
		prepared[i] = FolderSessionOptions{Device: certificates[i], Untrusted: true, EncryptionPassword: "test-concurrent-untrusted-password", PollInterval: 50 * time.Millisecond}
		if err := prepareFolderSessionOptions(folders[i], &prepared[i]); err != nil {
			t.Fatal(err)
		}
	}
	// Derive both secrets before releasing the two transport factories together.
	// A transient upstream first-token failure must be recovered by the service.
	ready, start := make(chan struct{}, 2), make(chan struct{})
	var sourceMu sync.Mutex
	var sourceStream net.Conn
	var sourceAttempts int
	services := make([]*FolderService, 2)
	for i := range folders {
		var entered sync.Once
		connect := func(ctx context.Context) (net.Conn, error) {
			entered.Do(func() { ready <- struct{}{} })
			select {
			case <-start:
			case <-ctx.Done():
				return nil, ctx.Err()
			}
			stream, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", reference.bep)
			if i == 0 {
				sourceMu.Lock()
				sourceStream = stream
				sourceAttempts++
				sourceMu.Unlock()
			}
			return stream, err
		}
		service, err := OpenFolderService(ctx, folders[i], FolderServiceOptions{Session: prepared[i], Connect: connect,
			RetryMin: 50 * time.Millisecond, RetryMax: 200 * time.Millisecond, OfflineInterval: 50 * time.Millisecond})
		if err != nil {
			t.Fatal(err)
		}
		services[i] = service
		t.Cleanup(func() {
			if err := service.Close(); err != nil {
				t.Error(err)
			}
		})
	}
	for range 2 {
		select {
		case <-ready:
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
	}
	close(start)
	waitFolderApplication(t, ctx, second, "tiny.txt", []byte("private first"))
	sourceMu.Lock()
	stream, attempts := sourceStream, sourceAttempts
	sourceMu.Unlock()
	if stream == nil {
		t.Fatal("source service did not connect")
	}
	stream.Close() // Actual transport loss; the next update needs a new session.
	if err := os.WriteFile(filepath.Join(first.options.Directory, "tiny.txt"), []byte("private after reconnect"), 0600); err != nil {
		t.Fatal(err)
	}
	waitFolderApplication(t, ctx, second, "tiny.txt", []byte("private after reconnect"))
	sourceMu.Lock()
	newAttempts := sourceAttempts
	sourceMu.Unlock()
	if newAttempts <= attempts {
		t.Fatal("encrypted service did not reconnect after transport loss")
	}
}
