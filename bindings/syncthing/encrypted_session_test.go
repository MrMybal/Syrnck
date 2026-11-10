package syncthing

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func sessionTestReceiver(t *testing.T, ctx context.Context, certificate tls.Certificate, peers []protocol.DeviceID) *EncryptedReceiver {
	t.Helper()
	storage := filepath.Join(tempDirectory(t), "opaque-store")
	store, err := syrnck.Open(storage, syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	receiver, err := NewEncryptedReceiver(ctx, EncryptedReceiverOptions{Storage: storage,
		Catalog: CatalogOptions{Folder: "project", LocalDevice: protocol.NewDeviceID(certificate.Certificate[0]), Peers: peers}})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { receiver.Close() })
	return receiver
}

func encryptedSessionPair(t *testing.T, ctx context.Context, receiver *EncryptedReceiver, receiverCertificate, peerCertificate tls.Certificate, peer *Folder, password string) (*FolderSession, *FolderSession) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	type opened struct {
		session *FolderSession
		err     error
	}
	ready := make(chan opened, 1)
	go func() {
		stream, err := listener.Accept()
		if err != nil {
			ready <- opened{nil, err}
			return
		}
		session, err := OpenEncryptedReceiverSession(ctx, receiver, stream, EncryptedReceiverSessionOptions{
			Device: DeviceOptions{Certificate: receiverCertificate, RemoteDevice: protocol.NewDeviceID(peerCertificate.Certificate[0]), Server: true}, PollInterval: 50 * time.Millisecond})
		ready <- opened{session, err}
	}()
	stream, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	client, err := OpenFolderSession(ctx, peer, stream, FolderSessionOptions{Untrusted: true, EncryptionPassword: password,
		Device: DeviceOptions{Certificate: peerCertificate, RemoteDevice: receiver.options.Catalog.LocalDevice}, PollInterval: 50 * time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	other := <-ready
	if other.err != nil {
		client.Close()
		t.Fatal(other.err)
	}
	t.Cleanup(func() { client.Close(); other.session.Close() })
	return client, other.session
}

func waitOpaqueEntry(t *testing.T, ctx context.Context, receiver *EncryptedReceiver, folderKey *[32]byte, name string, wanted []byte, sessions ...*FolderSession) CatalogEntry {
	t.Helper()
	keys := protocol.NewKeyGenerator()
	for {
		snapshot, err := receiver.Snapshot(ctx)
		if err == nil {
			for _, entry := range snapshot.Files {
				info, err := protocol.DecryptFileInfo(keys, entry.Info, folderKey)
				if err != nil {
					t.Fatal("receiver changed encrypted metadata", err)
				}
				if info.Name != name || info.Deleted || len(entry.CipherBlocks) != len(info.Blocks) {
					continue
				}
				if bytes.Equal(protocol.BlocksHash(info.Blocks), protocol.BlocksHash(plainTestInfo(name, wanted).Blocks)) {
					return entry
				}
			}
		}
		select {
		case <-ctx.Done():
			for i, session := range sessions {
				for {
					select {
					case err := <-session.Errors():
						t.Logf("session %d: %v", i, err)
					default:
						goto next
					}
				}
			next:
			}
			t.Fatal("opaque receipt timed out", name, ctx.Err(), err)
		case <-time.After(25 * time.Millisecond):
		}
	}
}

func plainTestInfo(name string, body []byte) protocol.FileInfo {
	info := protocol.FileInfo{Name: name, RawBlockSize: protocol.MinBlockSize}
	for offset := 0; offset < len(body) || offset == 0; offset += protocol.MinBlockSize {
		end := min(offset+protocol.MinBlockSize, len(body))
		piece := body[offset:end]
		hash := sha256.Sum256(piece)
		info.Blocks = append(info.Blocks, protocol.BlockInfo{Offset: int64(offset), Size: len(piece), Hash: hash[:]})
	}
	return info
}

func TestEncryptedReceiverSessionsRestoreWithoutSource(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	serverCert, sourceCert, restoreCert := deviceCertificate(t), deviceCertificate(t), deviceCertificate(t)
	serverID, sourceID, restoreID := protocol.NewDeviceID(serverCert.Certificate[0]), protocol.NewDeviceID(sourceCert.Certificate[0]), protocol.NewDeviceID(restoreCert.Certificate[0])
	receiver := sessionTestReceiver(t, ctx, serverCert, []protocol.DeviceID{sourceID, restoreID})
	root := tempDirectory(t)
	name := "Content/Map.uasset"
	before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	after := bytes.Join([][]byte{block(1), block(4), block(3)}, nil)
	files := map[string][]byte{name: before, "tiny.txt": []byte("small private payload"), "empty.txt": {}}
	source := createTestFolder(t, folderTestOptions(filepath.Join(root, "source-plain"), sourceID, serverID, false), files)
	const password = "test-native-opaque-folder-password"
	folderKey := protocol.NewKeyGenerator().KeyFromPassword("project", password)
	client, server := encryptedSessionPair(t, ctx, receiver, serverCert, sourceCert, source, password)
	old := waitOpaqueEntry(t, ctx, receiver, folderKey, name, before, client, server)
	for name, body := range files {
		waitOpaqueEntry(t, ctx, receiver, folderKey, name, body, client, server)
	}
	if err := os.WriteFile(filepath.Join(source.options.Directory, filepath.FromSlash(name)), after, 0600); err != nil {
		t.Fatal(err)
	}
	changed := waitOpaqueEntry(t, ctx, receiver, folderKey, name, after, client, server)
	if changed.CipherBlocks[0].Parts[0] != old.CipherBlocks[0].Parts[0] || changed.CipherBlocks[2].Parts[0] != old.CipherBlocks[2].Parts[0] || changed.CipherBlocks[1].Parts[0] == old.CipherBlocks[1].Parts[0] {
		t.Fatal("opaque delta failed to reuse unchanged ciphertext blocks")
	}
	if err := client.Close(); err != nil {
		t.Fatal(err)
	}
	if err := server.Close(); err != nil {
		t.Fatal(err)
	}
	opaqueTestGC(t, receiver, false)
	// Only the password-owning fresh device reconstructs plaintext. The source
	// and its network session have stopped before this device is connected.
	restore := createTestFolder(t, folderTestOptions(filepath.Join(root, "restore-encrypted"), restoreID, serverID, true), nil)
	restoredClient, restoredServer := encryptedSessionPair(t, ctx, receiver, serverCert, restoreCert, restore, password)
	files[name] = after
	for name, body := range files {
		waitFolderApplication(t, ctx, restore, name, body, restoredClient, restoredServer)
	}
	// An older advertised entry stays reconstructable after both delta and GC.
	for number, block := range old.Info.Blocks {
		if _, err := receiver.ReadRequest(ctx, old, &protocol.Request{Folder: "project", Name: old.Info.Name, Offset: block.Offset, Size: block.Size, Hash: block.Hash, BlockNo: number}); err != nil {
			t.Fatal("old encrypted revision lost", err)
		}
	}
}

func TestEncryptedReceiverWithReferenceTrustedDaemon(t *testing.T) {
	binary := os.Getenv("SYRNCK_SYNCTHING_BINARY")
	if binary == "" {
		t.Skip("set SYRNCK_SYNCTHING_BINARY for native opaque receiver interoperability")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	serverCert, restoreCert := deviceCertificate(t), deviceCertificate(t)
	serverID, restoreID := protocol.NewDeviceID(serverCert.Certificate[0]), protocol.NewDeviceID(restoreCert.Certificate[0])
	const password = "test-upstream-to-native-opaque-password"
	name := "Content/Map.uasset"
	body := bytes.Join([][]byte{block(3), block(1), []byte("private tail")}, nil)
	reference := startReferencePeersWithPasswords(t, ctx, binary, []protocol.DeviceID{serverID}, name, body, false, map[protocol.DeviceID]string{serverID: password})
	receiver := sessionTestReceiver(t, ctx, serverCert, []protocol.DeviceID{reference.device, restoreID})
	stream, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", reference.bep)
	if err != nil {
		t.Fatal(err)
	}
	session, err := OpenEncryptedReceiverSession(ctx, receiver, stream, EncryptedReceiverSessionOptions{Device: DeviceOptions{Certificate: serverCert, RemoteDevice: reference.device}, PollInterval: 50 * time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { session.Close() })
	folderKey := protocol.NewKeyGenerator().KeyFromPassword("project", password)
	waitOpaqueEntry(t, ctx, receiver, folderKey, name, body, session)
	if err := session.Close(); err != nil {
		t.Fatal(err)
	}
	opaqueTestGC(t, receiver, false)
	restore := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "restored-plain"), restoreID, serverID, false), nil)
	client, server := encryptedSessionPair(t, ctx, receiver, serverCert, restoreCert, restore, password)
	waitFolderApplication(t, ctx, restore, name, body, client, server)
	t.Log("actual upstream trusted daemon uploaded encrypted metadata/blocks; source disconnected; fresh Syrnck device restored from native opaque storage after GC")
}

func TestEncryptedReceiverSessionRejectsPlaintextAndForeignTokens(t *testing.T) {
	ctx := context.Background()
	peer := protocol.NewDeviceID([]byte("paired encrypted source"))
	receiver, _ := opaqueTestReceiver(t, false, []protocol.DeviceID{peer})
	session := &FolderSession{receiver: receiver, ctx: ctx,
		options: FolderSessionOptions{Device: DeviceOptions{RemoteDevice: peer}, QueueBytes: 1024 * 1024}, configured: make(chan struct{}), frames: make(chan folderIndexFrame, 1)}
	connection := folderModelConnection{device: peer}
	configuration := func() *protocol.ClusterConfig {
		return &protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project", Type: protocol.FolderTypeSendReceive, Devices: []protocol.Device{
			{ID: receiver.options.Catalog.LocalDevice, EncryptionPasswordToken: bytes.Repeat([]byte{7}, 32)}, {ID: peer, IndexID: 7},
		}}}}
	}
	before, err := receiver.Snapshot(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, change := range []func(*protocol.ClusterConfig){
		func(c *protocol.ClusterConfig) { c.Folders = nil },
		func(c *protocol.ClusterConfig) { c.Folders = append(c.Folders, c.Folders[0]) },
		func(c *protocol.ClusterConfig) { c.Folders[0].Type = protocol.FolderTypeReceiveEncrypted },
		func(c *protocol.ClusterConfig) { c.Folders[0].Devices[0].EncryptionPasswordToken = nil },
		func(c *protocol.ClusterConfig) { c.Folders[0].Devices[0].EncryptionPasswordToken[0]++ },
		func(c *protocol.ClusterConfig) {
			c.Folders[0].Devices[1].EncryptionPasswordToken = bytes.Repeat([]byte{7}, 32)
		},
		func(c *protocol.ClusterConfig) { c.Folders[0].Devices[1].IndexID = 0 },
	} {
		bad := configuration()
		change(bad)
		if err := session.ClusterConfig(connection, bad); err == nil {
			t.Fatal("invalid opaque agreement accepted")
		}
		after, err := receiver.Snapshot(ctx)
		if err != nil || after.Catalog != before.Catalog || session.authorized {
			t.Fatal("invalid agreement changed durable authorization", err)
		}
	}
	if err := session.ClusterConfig(folderModelConnection{device: protocol.NewDeviceID([]byte("foreign"))}, configuration()); err == nil {
		t.Fatal("foreign certificate configured receiver")
	}
	valid := opaqueTestInfo("private", [][]byte{bytes.Repeat([]byte{3}, 1064)}, 1, 1)
	batch := CatalogIndex{Reset: true, Files: []protocol.FileInfo{valid}, LastSequence: 1}
	if err := session.enqueue(connection, "project", batch); err == nil {
		t.Fatal("unauthorized encrypted index accepted")
	}
	if err := session.ClusterConfig(connection, configuration()); err != nil {
		t.Fatal(err)
	}
	changed := configuration()
	changed.Folders[0].Devices[1].IndexID++
	if err := session.ClusterConfig(connection, changed); err == nil {
		t.Fatal("peer changed its live generation")
	}
	for _, change := range []func(*protocol.FileInfo){
		func(i *protocol.FileInfo) { i.Name = "private.bin" },
		func(i *protocol.FileInfo) { i.Encrypted = nil },
		func(i *protocol.FileInfo) { i.Blocks[0].Hash = bytes.Repeat([]byte{1}, 32) },
		func(i *protocol.FileInfo) { i.RawBlockSize = protocol.MinBlockSize },
		func(i *protocol.FileInfo) { i.Platform.Unix = &protocol.UnixData{OwnerName: "private user"} },
		func(i *protocol.FileInfo) { i.PreviousBlocksHash = []byte("plaintext metadata") },
		func(i *protocol.FileInfo) { i.Encrypted = make([]byte, syrnck.MaxObjectSize) },
	} {
		bad := copyInfo(valid)
		change(&bad)
		if err := session.enqueue(connection, "project", CatalogIndex{Reset: true, Files: []protocol.FileInfo{bad}, LastSequence: 1}); err == nil || session.queuedBytes != 0 {
			t.Fatal("plaintext/malformed/oversized metadata entered encrypted queue", err)
		}
	}
	if err := session.enqueue(connection, "project", batch); err != nil {
		t.Fatal(err)
	}
	batch.Files[0].Encrypted[0]++
	frame := <-session.frames
	if frame.batch.Files[0].Encrypted[0] == batch.Files[0].Encrypted[0] || frame.batch.Device != peer || frame.batch.IndexID != 7 {
		t.Fatal("encrypted queue lost ownership or authenticated generation")
	}
}
