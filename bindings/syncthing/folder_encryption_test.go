package syncthing

import (
	"bytes"
	"context"
	"crypto/tls"
	"errors"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func TestFolderEncryptionAgreementAndDurableDowngradeGuard(t *testing.T) {
	ctx := context.Background()
	certificate := deviceCertificate(t)
	local, peer := protocol.NewDeviceID(certificate.Certificate[0]), protocol.NewDeviceID([]byte("encryption peer"))
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "encrypted"), local, peer, true), nil)
	options := FolderSessionOptions{Device: DeviceOptions{Certificate: certificate, RemoteDevice: peer},
		Untrusted: true, EncryptionPassword: "test-folder-password 密钥"}
	for _, password := range []string{"", strings.Repeat("x", 4097), "invalid\xff", "nul\x00password"} {
		bad := options
		bad.EncryptionPassword = password
		if err := prepareFolderSessionOptions(folder, &bad); err == nil {
			t.Fatal("invalid folder credential accepted")
		}
	}
	if err := prepareFolderSessionOptions(folder, &options); err != nil {
		t.Fatal(err)
	}
	keys := options.encryption.keys
	if err := prepareFolderSessionOptions(folder, &options); err != nil || options.encryption.keys != keys {
		t.Fatal("reconnection regenerated its key provider", err)
	}
	for _, localToken := range []bool{false, true} {
		own, remote := protocol.Device{ID: local}, protocol.Device{ID: peer}
		if localToken {
			own.EncryptionPasswordToken = bytes.Clone(options.encryption.token)
		} else {
			remote.EncryptionPasswordToken = bytes.Clone(options.encryption.token)
		}
		if err := checkFolderEncryption(options, &own, &remote); err != nil {
			t.Fatal(err)
		}
		if err := checkFolderEncryption(FolderSessionOptions{}, &own, &remote); err == nil {
			t.Fatal("plaintext mode accepted encrypted agreement")
		}
	}
	for _, tokens := range [][2][]byte{{nil, nil}, {[]byte("bad"), nil},
		{options.encryption.token, options.encryption.token}} {
		if err := checkFolderEncryption(options, &protocol.Device{EncryptionPasswordToken: tokens[0]},
			&protocol.Device{EncryptionPasswordToken: tokens[1]}); err == nil {
			t.Fatal("invalid encryption echo accepted")
		}
	}
	plain := options
	plain.Untrusted, plain.EncryptionPassword, plain.encryption = false, "", nil
	if err := folder.bindEncryption(ctx, plain); err != nil {
		t.Fatal(err)
	}
	if err := folder.bindEncryption(ctx, options); err != nil {
		t.Fatal("explicit encrypted upgrade", err)
	}
	var before map[string]syrnck.ObjectKey
	if err := folder.withStore(ctx, func(store *syrnck.Store) error { var err error; before, err = store.Refs(); return err }); err != nil {
		t.Fatal(err)
	}
	if err := folder.bindEncryption(ctx, options); err != nil {
		t.Fatal(err)
	}
	if err := folder.bindEncryption(ctx, plain); err == nil {
		t.Fatal("durable encryption downgraded")
	}
	changed := options
	changed.EncryptionPassword, changed.encryption = "other-password", nil
	if err := prepareFolderSessionOptions(folder, &changed); err != nil {
		t.Fatal(err)
	}
	if err := folder.bindEncryption(ctx, changed); err == nil {
		t.Fatal("durable encryption changed secret")
	}
	if err := folder.withStore(ctx, func(store *syrnck.Store) error {
		after, err := store.Refs()
		if err != nil {
			return err
		}
		if len(after) != len(before) {
			t.Fatal("rejected change mutated refs")
		}
		for name, key := range before {
			if after[name] != key {
				t.Fatal("encryption checkpoint changed")
			}
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	// A service must reject the persisted downgrade before dialing/listening.
	calls := 0
	_, err := OpenFolderService(ctx, folder, FolderServiceOptions{Session: plain, Connect: func(context.Context) (net.Conn, error) { calls++; return nil, errors.New("must not connect") }})
	if err == nil || calls != 0 {
		t.Fatal("downgraded service opened a transport", calls, err)
	}
}

func TestEncryptedFolderCannotAuthorizeBeforeCodecInstallation(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	certificate := deviceCertificate(t)
	local, peer := protocol.NewDeviceID(certificate.Certificate[0]), protocol.NewDeviceID([]byte("fast encrypted peer"))
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), local, peer, false),
		map[string][]byte{"private.txt": []byte("PRIVATE-BEFORE-CODEC")})
	options := FolderSessionOptions{Device: DeviceOptions{Certificate: certificate, RemoteDevice: peer},
		Untrusted: true, EncryptionPassword: "test-fast-peer-password", QueueBytes: 1024 * 1024}
	if err := prepareFolderSessionOptions(folder, &options); err != nil {
		t.Fatal(err)
	}
	snapshot := folderSnapshot(t, folder)
	session := &FolderSession{folder: folder, options: options, ctx: ctx, codecReady: make(chan struct{}), configured: make(chan struct{}), frames: make(chan folderIndexFrame, 1)}
	connection := folderModelConnection{device: peer}
	config := &protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project", Devices: []protocol.Device{
		{ID: local, IndexID: protocol.IndexID(snapshot.IndexID)},
		{ID: peer, IndexID: 7, EncryptionPasswordToken: bytes.Clone(options.encryption.token)},
	}}}}
	entered, done := make(chan struct{}), make(chan error, 1)
	var worker sync.WaitGroup
	worker.Add(1)
	go func() { defer worker.Done(); close(entered); done <- session.ClusterConfig(connection, config) }()
	defer worker.Wait()
	defer cancel()
	<-entered
	select {
	case err := <-done:
		t.Fatal("peer authorized before codec installation", err)
	case <-time.After(20 * time.Millisecond):
	}
	if _, err := session.Request(connection, &protocol.Request{Folder: "project", Name: "private.txt", Size: 20}); err == nil {
		t.Fatal("early plaintext block request was served")
	}
	if err := session.Index(connection, &protocol.Index{Folder: "project", Files: []protocol.FileInfo{snapshot.Files[0].Info}, LastSequence: 1}); err == nil || len(session.frames) != 0 {
		t.Fatal("unauthenticated metadata reached the queue before codec installation", err)
	}
	close(session.codecReady)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
}

func TestReferenceUntrustedDaemonEncryptedRoundtripAndDifferentialBlocks(t *testing.T) {
	binary := os.Getenv("SYRNCK_SYNCTHING_BINARY")
	if binary == "" {
		t.Skip("set SYRNCK_SYNCTHING_BINARY for untrusted-device qualification")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	firstCert, secondCert, thirdCert := deviceCertificate(t), deviceCertificate(t), deviceCertificate(t)
	firstID, secondID, thirdID := protocol.NewDeviceID(firstCert.Certificate[0]), protocol.NewDeviceID(secondCert.Certificate[0]), protocol.NewDeviceID(thirdCert.Certificate[0])
	reference := startReferencePeers(t, ctx, binary, []protocol.DeviceID{firstID, secondID, thirdID}, "", nil, true)
	name := "Content/Private.uasset"
	before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	after := bytes.Join([][]byte{block(1), block(4), block(3)}, nil)
	inputs := map[string][]byte{name: before, "small.txt": []byte("PRIVATE-SMALL"), "empty.txt": {},
		"short-final.bin": append(bytes.Clone(block(7)), []byte("END")...)}
	root := tempDirectory(t)
	first := createTestFolder(t, folderTestOptions(filepath.Join(root, "first-encrypted"), firstID, reference.device, true), inputs)
	second := createTestFolder(t, folderTestOptions(filepath.Join(root, "second"), secondID, reference.device, false), nil)
	connect := func(folder *Folder, certificate tls.Certificate) *FolderSession {
		t.Helper()
		stream, err := net.DialTimeout("tcp", reference.bep, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		session, err := OpenFolderSession(ctx, folder, stream, FolderSessionOptions{Device: DeviceOptions{
			Certificate: certificate, RemoteDevice: reference.device}, PollInterval: 100 * time.Millisecond,
			Untrusted: true, EncryptionPassword: "test-native-untrusted-password 密钥"})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() {
			if err := session.Close(); err != nil {
				t.Error(err)
			}
		})
		return session
	}
	session := connect(first, firstCert)
	// A single-session caller waits for completed password enrollment. The
	// upstream token file can be read halfway through its first write by a
	// concurrent second enrollment; the service reconnect test covers that case.
	select {
	case <-session.configured:
	case <-ctx.Done():
		t.Fatal("source encryption was not confirmed", ctx.Err())
	}
	// Receiving via another trusted endpoint proves the unmodified daemon can
	// retain and serve opaque metadata/data without possessing the password.
	other := connect(second, secondCert)
	for path, data := range inputs {
		waitFolderApplication(t, ctx, second, path, data, session, other)
	}
	if err := other.Close(); err != nil {
		t.Fatal(err)
	}
	baseline := session.connection.Statistics().OutBytesTotal
	if err := os.WriteFile(filepath.Join(first.options.Directory, filepath.FromSlash(name)), after, 0600); err != nil {
		t.Fatal(err)
	}
	// Reconnect a receiver retaining its original manifests: only the changed
	// block at the same offset should be fetched from the opaque daemon.
	other = connect(second, secondCert)
	waitFolderApplication(t, ctx, second, name, after, session, other)
	sent := session.connection.Statistics().OutBytesTotal - baseline
	if sent < int64(protocol.MinBlockSize) || sent >= 2*int64(protocol.MinBlockSize) {
		t.Fatalf("encrypted delta retransferred unchanged blocks: %d protocol bytes", sent)
	}
	if err := session.Close(); err != nil {
		t.Fatal(err)
	}
	if err := other.Close(); err != nil {
		t.Fatal(err)
	}
	// No cleartext path or payload is present in the untrusted folder itself.
	var count int
	if err := filepath.WalkDir(reference.folder, func(path string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() {
			return nil
		}
		if strings.Contains(path, ".stfolder") {
			return nil
		}
		if strings.Contains(path, "Private") || strings.Contains(path, "small.txt") {
			t.Fatal("plaintext path reached untrusted storage")
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if bytes.Contains(data, []byte("PRIVATE-SMALL")) || bytes.Contains(data, []byte(name)) || bytes.Contains(data, before[:1024]) {
			t.Fatal("plaintext bytes reached untrusted storage")
		}
		count++
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	if count != len(inputs) {
		t.Fatalf("encrypted receiver retained %d files, want %d", count, len(inputs))
	}
	// A fresh trusted device restores solely from retained untrusted storage,
	// with both earlier trusted endpoints now disconnected.
	third := createTestFolder(t, folderTestOptions(filepath.Join(root, "third-encrypted"), thirdID, reference.device, true), nil)
	other = connect(third, thirdCert)
	for path, data := range inputs {
		if path == name {
			data = after
		}
		waitFolderApplication(t, ctx, third, path, data, other)
	}
	t.Logf("native encrypted-device protocol: short/empty/tail blocks, restart and delta %d bytes", sent)
}

type untrustedAttackModel struct {
	*catalogDaemonModel
	local, source protocol.DeviceID
}

func (m *untrustedAttackModel) ClusterConfig(connection protocol.Connection, config *protocol.ClusterConfig) error {
	var token []byte
	for _, folder := range config.Folders {
		for _, device := range folder.Devices {
			if device.ID == m.local {
				token = bytes.Clone(device.EncryptionPasswordToken)
			}
		}
	}
	if len(token) == 0 {
		return errors.New("test source omitted encryption requirement")
	}
	connection.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project", Devices: []protocol.Device{
		{ID: m.local, IndexID: 9, EncryptionPasswordToken: token}, {ID: m.source},
	}}}}, nil)
	return nil
}

func TestEncryptedFolderRejectsPlaintextAndTamperedMetadataOverTLS(t *testing.T) {
	for _, attack := range []string{"plaintext", "tampered"} {
		t.Run(attack, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			certificate, remoteCert := deviceCertificate(t), deviceCertificate(t)
			local, remote := protocol.NewDeviceID(certificate.Certificate[0]), protocol.NewDeviceID(remoteCert.Certificate[0])
			folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), local, remote, false), map[string][]byte{"secret.txt": []byte("PRIVATE-IMMUTABLE")})
			original := folderSnapshot(t, folder)
			left, right := streams(t)
			model := &untrustedAttackModel{local: remote, source: local, catalogDaemonModel: &catalogDaemonModel{
				peer: local, configured: make(chan uint64, 1), batches: make(chan CatalogIndex, 16), closed: make(chan error, 1)}}
			type opened struct {
				connection *DeviceConnection
				err        error
			}
			ready := make(chan opened, 1)
			go func() {
				connection, err := OpenDeviceConnection(ctx, right, DeviceOptions{Certificate: remoteCert, RemoteDevice: local, Server: true}, model)
				if err == nil {
					connection.Start()
				}
				ready <- opened{connection, err}
			}()
			session, err := OpenFolderSession(ctx, folder, left, FolderSessionOptions{Device: DeviceOptions{Certificate: certificate, RemoteDevice: remote},
				Untrusted: true, EncryptionPassword: "test-attack-folder-password", PollInterval: 50 * time.Millisecond})
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { session.Close() })
			peer := <-ready
			if peer.err != nil {
				t.Fatal(peer.err)
			}
			t.Cleanup(func() { peer.connection.Close(nil) })
			var encrypted protocol.FileInfo
			select {
			case batch := <-model.batches:
				if len(batch.Files) != 1 {
					t.Fatal("missing source index")
				}
				encrypted = batch.Files[0]
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			if encrypted.Name == "secret.txt" || len(encrypted.Encrypted) == 0 {
				t.Fatal("source sent plaintext metadata")
			}
			info := encrypted
			if attack == "tampered" {
				info.Encrypted = bytes.Clone(info.Encrypted)
				info.Encrypted[0] ^= 1
			} else {
				info = copyInfo(original.Files[0].Info)
			}
			info.Sequence = 1
			if err := peer.connection.Index(ctx, &protocol.Index{Folder: "project", Files: []protocol.FileInfo{info}, LastSequence: 1}); err != nil {
				t.Fatal(err)
			}
			select {
			case <-session.Done():
			case <-ctx.Done():
				t.Fatal("invalid ciphertext was not rejected", ctx.Err())
			}
			snapshot := folderSnapshot(t, folder)
			if snapshot.Catalog != original.Catalog {
				t.Fatal("unauthenticated metadata changed the durable catalog")
			}
			data, err := os.ReadFile(filepath.Join(folder.options.Directory, "secret.txt"))
			if err != nil || !bytes.Equal(data, []byte("PRIVATE-IMMUTABLE")) {
				t.Fatal("rejected metadata changed workspace", err)
			}
		})
	}
}
