package syncthing

import (
	"bytes"
	"context"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

func TestEncryptedReceiverConcurrentServicesReconnect(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	serverCert := deviceCertificate(t)
	certs := []DeviceOptions{{Certificate: deviceCertificate(t)}, {Certificate: deviceCertificate(t)}}
	serverID := protocol.NewDeviceID(serverCert.Certificate[0])
	peers := []protocol.DeviceID{protocol.NewDeviceID(certs[0].Certificate.Certificate[0]), protocol.NewDeviceID(certs[1].Certificate.Certificate[0])}
	receiver := sessionTestReceiver(t, ctx, serverCert, peers)
	root := tempDirectory(t)
	name := "asset.bin"
	before, after := bytes.Join([][]byte{block(1), block(2)}, nil), bytes.Join([][]byte{block(3), block(2)}, nil)
	folders := []*Folder{
		createTestFolder(t, folderTestOptions(filepath.Join(root, "source-plain"), peers[0], serverID, false), map[string][]byte{name: before}),
		createTestFolder(t, folderTestOptions(filepath.Join(root, "target-encrypted"), peers[1], serverID, true), nil),
	}
	var sourceMu sync.Mutex
	var sourceStream net.Conn
	var sourceAttempts int
	for i, folder := range folders {
		listener, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { listener.Close() })
		server, err := OpenEncryptedReceiverService(ctx, receiver, EncryptedReceiverServiceOptions{
			Session: EncryptedReceiverSessionOptions{Device: DeviceOptions{Certificate: serverCert, RemoteDevice: peers[i], Server: true}, PollInterval: 50 * time.Millisecond},
			Connect: func(ctx context.Context) (net.Conn, error) {
				stop := context.AfterFunc(ctx, func() { listener.Close() })
				defer stop()
				return listener.Accept()
			}, RetryMin: 50 * time.Millisecond, RetryMax: 200 * time.Millisecond, OfflineInterval: 50 * time.Millisecond})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { server.Close() })
		device := certs[i]
		device.RemoteDevice = serverID
		client, err := OpenFolderService(ctx, folder, FolderServiceOptions{
			Session: FolderSessionOptions{Device: device, Untrusted: true, EncryptionPassword: "test-shared-opaque-password", PollInterval: 50 * time.Millisecond},
			Connect: func(ctx context.Context) (net.Conn, error) {
				stream, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", listener.Addr().String())
				if i == 0 {
					sourceMu.Lock()
					sourceStream, sourceAttempts = stream, sourceAttempts+1
					sourceMu.Unlock()
				}
				return stream, err
			}, RetryMin: 50 * time.Millisecond, RetryMax: 200 * time.Millisecond, OfflineInterval: 50 * time.Millisecond})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { client.Close() })
	}
	waitFolderApplication(t, ctx, folders[1], name, before)
	sourceMu.Lock()
	stream, attempts := sourceStream, sourceAttempts
	sourceMu.Unlock()
	if stream == nil {
		t.Fatal("source never connected")
	}
	stream.Close() // Lose the live authenticated stream; both services reconnect.
	if err := os.WriteFile(filepath.Join(folders[0].options.Directory, name), after, 0600); err != nil {
		t.Fatal(err)
	}
	waitFolderBytes(t, ctx, filepath.Join(folders[1].options.Directory, name), after)
	sourceMu.Lock()
	reconnected := sourceAttempts > attempts
	sourceMu.Unlock()
	if !reconnected {
		t.Fatal("opaque service did not reconnect after actual stream loss")
	}
}
