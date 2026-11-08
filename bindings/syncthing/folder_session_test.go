package syncthing

import (
	"bytes"
	"context"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

func waitFolderBytes(t *testing.T, ctx context.Context, path string, wanted []byte, sessions ...*FolderSession) {
	t.Helper()
	for {
		data, err := os.ReadFile(path)
		if err == nil && bytes.Equal(data, wanted) {
			return
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
			t.Fatal("working-folder synchronization timed out", ctx.Err(), err)
		case <-time.After(25 * time.Millisecond):
		}
	}
}

// Files become visible before the native transaction publishes its receipt.
// An external edit in that earlier window is intentionally refused by native
// recovery and requires preservation/resolution. Exercise post-completion edits
// only after both physical data and the durable protocol checkpoint are ready.
func waitFolderApplication(t *testing.T, ctx context.Context, folder *Folder, name string, wanted []byte, sessions ...*FolderSession) {
	t.Helper()
	waitFolderBytes(t, ctx, filepath.Join(folder.options.Directory, filepath.FromSlash(name)), wanted, sessions...)
	for {
		snapshot, err := folder.Snapshot(ctx)
		if err == nil {
			for _, entry := range snapshot.Files {
				if entry.Info.Name == name && !entry.Info.Deleted && entry.Manifest != "" {
					return
				}
			}
		}
		select {
		case <-ctx.Done():
			t.Fatal("working application was not acknowledged", ctx.Err(), err)
		case <-time.After(25 * time.Millisecond):
		}
	}
}

func TestFolderSessionsSynchronizeWorkingTreesOverPinnedTLS(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	leftCert, rightCert := deviceCertificate(t), deviceCertificate(t)
	leftID, rightID := protocol.NewDeviceID(leftCert.Certificate[0]), protocol.NewDeviceID(rightCert.Certificate[0])
	root := tempDirectory(t)
	name := "Content/Map.uasset"
	before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
	left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left-plain"), leftID, rightID, false), map[string][]byte{name: before})
	right := createTestFolder(t, folderTestOptions(filepath.Join(root, "right-plain"), rightID, leftID, false), nil)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	accepted := make(chan net.Conn, 1)
	go func() { stream, _ := listener.Accept(); accepted <- stream }()
	leftStream, err := net.DialTimeout("tcp", listener.Addr().String(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	rightStream := <-accepted
	if rightStream == nil {
		t.Fatal("loopback accept failed")
	}
	type opened struct {
		session *FolderSession
		err     error
	}
	ready := make(chan opened, 1)
	go func() {
		session, err := OpenFolderSession(ctx, right, rightStream, FolderSessionOptions{Device: DeviceOptions{Certificate: rightCert, RemoteDevice: leftID, Server: true}, PollInterval: 100 * time.Millisecond})
		ready <- opened{session, err}
	}()
	leftSession, err := OpenFolderSession(ctx, left, leftStream, FolderSessionOptions{Device: DeviceOptions{Certificate: leftCert, RemoteDevice: rightID}, PollInterval: 100 * time.Millisecond})
	if err != nil {
		t.Fatal(err)
	}
	other := <-ready
	if other.err != nil {
		t.Fatal(other.err)
	}
	rightSession := other.session
	t.Cleanup(func() {
		cancel()
		if err := leftSession.Close(); err != nil {
			t.Error(err)
		}
		if err := rightSession.Close(); err != nil {
			t.Error(err)
		}
	})
	waitFolderApplication(t, ctx, right, name, before, leftSession, rightSession)
	if err := os.WriteFile(filepath.Join(right.options.Directory, filepath.FromSlash(name)), after, 0600); err != nil {
		t.Fatal(err)
	}
	waitFolderBytes(t, ctx, filepath.Join(left.options.Directory, filepath.FromSlash(name)), after, leftSession, rightSession)
	// Metadata and sync workers must serve simultaneous block requests without
	// holding the BEP reader or leaving a Store locked between polling ticks.
	newName := "Source/new.cpp"
	newData := []byte("int revision = 1;\n")
	if err := os.MkdirAll(filepath.Join(left.options.Directory, "Source"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(left.options.Directory, filepath.FromSlash(newName)), newData, 0600); err != nil {
		t.Fatal(err)
	}
	waitFolderBytes(t, ctx, filepath.Join(right.options.Directory, filepath.FromSlash(newName)), newData, leftSession, rightSession)
}

// This test uses the production continuous controller as the real upstream
// daemon's peer. No test protocol.Model drives indexes, vectors or application.
func TestFolderSessionWithReferenceDaemon(t *testing.T) {
	binary := os.Getenv("SYRNCK_SYNCTHING_BINARY")
	if binary == "" {
		t.Skip("set SYRNCK_SYNCTHING_BINARY for continuous real-daemon folder qualification")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 35*time.Second)
	defer cancel()
	certificate := deviceCertificate(t)
	local := protocol.NewDeviceID(certificate.Certificate[0])
	name, renamed := "Content/Map.uasset", "Content/Renamed.uasset"
	before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
	reference := startReference(t, ctx, binary, local, name, before)
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "engine-encrypted"), local, reference.device, true), nil)
	connect := func() *FolderSession {
		stream, err := net.DialTimeout("tcp", reference.bep, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		session, err := OpenFolderSession(ctx, folder, stream, FolderSessionOptions{Device: DeviceOptions{Certificate: certificate, RemoteDevice: reference.device}, PollInterval: 100 * time.Millisecond})
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
	session := connect()
	path := filepath.Join(folder.options.Directory, filepath.FromSlash(name))
	waitFolderApplication(t, ctx, folder, name, before, session)
	if err := os.WriteFile(path, after, 0600); err != nil {
		t.Fatal(err)
	}
	reference.waitFile(t, ctx, name, after)
	if err := session.Close(); err != nil {
		t.Fatal(err)
	}
	// An offline upstream rename must resume from the durable frontier and
	// change actual native working files without re-downloading identical data.
	if err := os.Rename(filepath.Join(reference.folder, filepath.FromSlash(name)), filepath.Join(reference.folder, filepath.FromSlash(renamed))); err != nil {
		t.Fatal(err)
	}
	reference.scan(t, ctx)
	session = connect()
	waitFolderApplication(t, ctx, folder, renamed, after, session)
	for {
		_, err := os.Stat(path)
		if os.IsNotExist(err) {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatal("old rename path not removed", ctx.Err())
		case <-time.After(25 * time.Millisecond):
		}
	}
	// Close before the external offline deletion so Windows native journal
	// handles cannot race this test's ordinary os.Remove call.
	if err := session.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(filepath.Join(folder.options.Directory, filepath.FromSlash(renamed))); err != nil {
		t.Fatal(err)
	}
	session = connect()
	for {
		_, err := os.Stat(filepath.Join(reference.folder, filepath.FromSlash(renamed)))
		if os.IsNotExist(err) {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatal("local tombstone not applied upstream", ctx.Err())
		case <-time.After(25 * time.Millisecond):
		}
	}
	t.Log("production FolderSession: real-daemon initial receipt, post-completion local edit, offline rename/reconnect, native deletion propagated upstream")
}

type folderModelConnection struct {
	protocol.Connection
	device protocol.DeviceID
}

func (c folderModelConnection) DeviceID() protocol.DeviceID { return c.device }

func TestFolderSessionAuthorizesFolderAndBoundsQueue(t *testing.T) {
	local, peer := protocol.NewDeviceID([]byte("queue local")), protocol.NewDeviceID([]byte("queue peer"))
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "queue"), local, peer, false), map[string][]byte{"asset": []byte("verified content")})
	snapshot := folderSnapshot(t, folder)
	session := &FolderSession{folder: folder, options: FolderSessionOptions{Device: DeviceOptions{RemoteDevice: peer}, QueueBytes: 1024 * 1024}, configured: make(chan struct{}), frames: make(chan folderIndexFrame, 1)}
	connection := folderModelConnection{device: peer}
	batch := CatalogIndex{Reset: true, Files: []protocol.FileInfo{snapshot.Files[0].Info}, LastSequence: snapshot.Sequence}
	if err := session.enqueue(connection, "project", batch); err == nil {
		t.Fatal("unnegotiated folder accepted index")
	}
	configuration := func() *protocol.ClusterConfig {
		return &protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project", Type: protocol.FolderTypeSendReceive, Devices: []protocol.Device{{ID: local, IndexID: protocol.IndexID(snapshot.IndexID)}, {ID: peer, IndexID: 7}}}}}
	}
	wrong := folderModelConnection{device: protocol.NewDeviceID([]byte("unpaired peer"))}
	if err := session.ClusterConfig(wrong, configuration()); err == nil {
		t.Fatal("foreign connection configured folder")
	}
	invalid := configuration()
	invalid.Folders[0].Type = protocol.FolderTypeReceiveEncrypted
	if err := session.ClusterConfig(connection, invalid); err == nil {
		t.Fatal("receive-encrypted folder accepted")
	}
	invalid = configuration()
	invalid.Folders[0].Devices[1].IndexID = 0
	if err := session.ClusterConfig(connection, invalid); err == nil {
		t.Fatal("zero peer generation accepted")
	}
	if err := session.ClusterConfig(connection, configuration()); err != nil {
		t.Fatal(err)
	}
	invalid = configuration()
	invalid.Folders[0].Devices[1].IndexID = 8
	if err := session.ClusterConfig(connection, invalid); err == nil {
		t.Fatal("live generation changed")
	}
	if err := session.enqueue(wrong, "project", batch); err == nil {
		t.Fatal("foreign connection queued index")
	}
	if err := session.enqueue(connection, "other", batch); err == nil {
		t.Fatal("unauthorized folder queued index")
	}
	malformed := batch
	malformed.Files = []protocol.FileInfo{copyInfo(batch.Files[0])}
	malformed.Files[0].Name = "../escape"
	if err := session.enqueue(connection, "project", malformed); err == nil {
		t.Fatal("unsafe path queued")
	}
	if err := session.enqueue(connection, "project", batch); err != nil {
		t.Fatal(err)
	}
	queued := session.queuedBytes
	if err := session.enqueue(connection, "project", batch); err == nil {
		t.Fatal("full queue accepted a frame")
	}
	if session.queuedBytes != queued {
		t.Fatal("rejected frame leaked queue budget")
	}
	frame := <-session.frames
	if frame.batch.Device != peer || frame.batch.IndexID != 7 {
		t.Fatal("queued frame lost authenticated identity")
	}
	// The queue owns a copy; caller mutation cannot rewrite an accepted index.
	batch.Files[0].Name = "mutated"
	if frame.batch.Files[0].Name != "asset" {
		t.Fatal("queue retained caller metadata")
	}
	session.queuedBytes = session.options.QueueBytes
	if err := session.enqueue(connection, "project", batch); err == nil {
		t.Fatal("exhausted byte budget accepted index")
	}
}
