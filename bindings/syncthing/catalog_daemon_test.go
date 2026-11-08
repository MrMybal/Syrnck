package syncthing

import (
	"bytes"
	"context"
	"crypto/tls"
	"errors"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

// The model queues the real daemon's original sequence-bounded messages.
// All engine handles are closed while awaiting messages or downloading bytes.
type catalogDaemonModel struct {
	peer       protocol.DeviceID
	generation uint64
	configured chan uint64
	batches    chan CatalogIndex
	closed     chan error
}

func (m *catalogDaemonModel) ClusterConfig(_ protocol.Connection, config *protocol.ClusterConfig) error {
	for _, folder := range config.Folders {
		if folder.ID == "project" {
			for _, device := range folder.Devices {
				if device.ID == m.peer {
					m.generation = uint64(device.IndexID)
					if m.generation == 0 {
						return errors.New("reference peer did not advertise an index generation")
					}
					m.configured <- m.generation
					return nil
				}
			}
		}
	}
	return errors.New("reference peer did not authorize the test folder")
}
func (m *catalogDaemonModel) Index(_ protocol.Connection, index *protocol.Index) error {
	return m.receive(index.Folder, CatalogIndex{Device: m.peer, IndexID: m.generation, Reset: true, LastSequence: index.LastSequence, Files: index.Files})
}
func (m *catalogDaemonModel) IndexUpdate(_ protocol.Connection, index *protocol.IndexUpdate) error {
	return m.receive(index.Folder, CatalogIndex{Device: m.peer, IndexID: m.generation, PrevSequence: index.PrevSequence, LastSequence: index.LastSequence, Files: index.Files})
}
func (m *catalogDaemonModel) receive(folder string, batch CatalogIndex) error {
	if folder != "project" {
		return protocol.ErrNoSuchFile
	}
	files := make([]protocol.FileInfo, len(batch.Files))
	for i, info := range batch.Files {
		files[i] = copyInfo(info)
	}
	batch.Files = files
	select {
	case m.batches <- batch:
		return nil
	default:
		return errors.New("reference test queue exhausted")
	}
}
func (m *catalogDaemonModel) Closed(_ protocol.Connection, err error) {
	select {
	case m.closed <- err:
	default:
	}
}
func (*catalogDaemonModel) DownloadProgress(protocol.Connection, *protocol.DownloadProgress) error {
	return nil
}
func (*catalogDaemonModel) Request(protocol.Connection, *protocol.Request) (protocol.RequestResponse, error) {
	return nil, protocol.ErrNoSuchFile
}

func TestReferenceDaemonDurableFolderReconnection(t *testing.T) {
	binary := os.Getenv("SYRNCK_SYNCTHING_BINARY")
	if binary == "" {
		t.Skip("set SYRNCK_SYNCTHING_BINARY to qualify durable real-daemon indexes")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	certificate := deviceCertificate(t)
	local := protocol.NewDeviceID(certificate.Certificate[0])
	name := "Content/Édit.uasset"
	renamed := "Content/Renamed.uasset"
	before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
	reference := startReference(t, ctx, binary, local, name, before)
	options := CatalogOptions{Folder: "project", LocalDevice: local, Peers: []protocol.DeviceID{reference.device}}
	root := filepath.Join(tempDirectory(t), "engine")
	key := bytes.Repeat([]byte{42}, 32)
	open := func(create bool) *syrnck.Store {
		store, err := syrnck.Open(root, syrnck.Options{Create: create, Key: key})
		if err != nil {
			t.Fatal(err)
		}
		return store
	}
	store := open(true)
	catalog := mustCatalog(t, store, options)
	mustCommitCatalog(t, store, catalog)
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	connect := func() (*DeviceConnection, *catalogDaemonModel) {
		store := open(false)
		catalog := mustCatalog(t, store, options)
		localGeneration, localSequence, localFiles := catalog.LocalIndex()
		peerGeneration, peerSequence, _, err := catalog.PeerIndex(reference.device)
		if err != nil {
			t.Fatal(err)
		}
		if err := store.Close(); err != nil {
			t.Fatal(err)
		}
		model := &catalogDaemonModel{peer: reference.device, configured: make(chan uint64, 1), batches: make(chan CatalogIndex, 128), closed: make(chan error, 1)}
		stream, err := net.DialTimeout("tcp", reference.bep, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		connection, err := OpenDeviceConnection(ctx, stream, DeviceOptions{Certificate: tls.Certificate(certificate), RemoteDevice: reference.device}, model)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { connection.Close(nil) })
		connection.Start()
		connection.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project", Type: protocol.FolderTypeSendReceive, Devices: []protocol.Device{
			{ID: local, IndexID: protocol.IndexID(localGeneration), MaxSequence: localSequence},
			{ID: reference.device, IndexID: protocol.IndexID(peerGeneration), MaxSequence: peerSequence},
		}}}}, nil)
		select {
		case <-model.configured:
		case err := <-model.closed:
			t.Fatal(err)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
		files := make([]protocol.FileInfo, len(localFiles))
		for i, entry := range localFiles {
			files[i] = entry.Info
		}
		if err := connection.Index(ctx, &protocol.Index{Folder: "project", Files: files, LastSequence: localSequence}); err != nil {
			t.Fatal(err)
		}
		return connection, model
	}
	connection, model := connect()
	persistNext := func(model *catalogDaemonModel) CatalogIndex {
		var batch CatalogIndex
		select {
		case batch = <-model.batches:
		case err := <-model.closed:
			t.Fatal(err)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
		store := open(false)
		catalog := mustCatalog(t, store, options)
		if err := catalog.ReceiveIndex(batch); err != nil {
			t.Fatalf("real-daemon batch reset=%v prev=%d last=%d: %v", batch.Reset, batch.PrevSequence, batch.LastSequence, err)
		}
		for _, info := range batch.Files {
			if info.Deleted || info.Type != protocol.FileInfoTypeFile {
				if err := catalog.Adopt(reference.device, info.Name); err != nil {
					t.Fatal(err)
				}
			}
		}
		mustCommitCatalog(t, store, catalog)
		if err := store.Close(); err != nil {
			t.Fatal(err)
		}
		return batch
	}
	waitName := func(model *catalogDaemonModel, wanted string) protocol.FileInfo {
		for {
			batch := persistNext(model)
			for _, info := range batch.Files {
				if protocolName(info.Name) == wanted && !info.Deleted && info.Type == protocol.FileInfoTypeFile {
					return info
				}
			}
		}
	}
	apply := func(info protocol.FileInfo, reuse *CatalogEntry) CatalogEntry {
		download := DownloadOptions{}
		if reuse != nil {
			download.ReuseIndex = &reuse.Info
			download.ReadReuse = func(ctx context.Context, req *protocol.Request) ([]byte, error) {
				store := open(false)
				defer store.Close()
				view := &File{store: store, manifest: reuse.Manifest, folder: "project", info: reuse.Info}
				return view.ReadRequest(ctx, req)
			}
		}
		staged, err := DownloadFile(ctx, "project", info, connection.Request, download)
		if err != nil {
			t.Fatal(err)
		}
		defer staged.Close()
		if reuse != nil && staged.Stats().Requests > 1 {
			t.Fatalf("durable catalog did not reuse data: %+v", staged.Stats())
		}
		store := open(false)
		defer store.Close()
		catalog := mustCatalog(t, store, options)
		result, err := staged.Prepare(ctx, store)
		if err != nil {
			t.Fatal(err)
		}
		if err := catalog.Attach(ctx, store, reference.device, info.Name, result.Manifest); err != nil {
			t.Fatal(err)
		}
		if err := catalog.Adopt(reference.device, info.Name); err != nil {
			t.Fatal(err)
		}
		mustCommitCatalog(t, store, catalog)
		return localEntry(t, catalog, protocolName(info.Name))
	}
	original := apply(waitName(model, name), nil)
	if err := os.WriteFile(filepath.Join(reference.folder, filepath.FromSlash(name)), after, 0600); err != nil {
		t.Fatal(err)
	}
	modified := time.Now().Add(3 * time.Second)
	if err := os.Chtimes(filepath.Join(reference.folder, filepath.FromSlash(name)), modified, modified); err != nil {
		t.Fatal(err)
	}
	reference.scan(t, ctx)
	updated := apply(waitName(model, name), &original)
	connection.Close(nil)
	select {
	case <-model.closed:
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	// The engine's native Store and BEP session are both closed before this
	// offline rename. Reconnecting must resume the exact persisted frontier.
	if err := os.Rename(filepath.Join(reference.folder, filepath.FromSlash(name)), filepath.Join(reference.folder, filepath.FromSlash(renamed))); err != nil {
		t.Fatal(err)
	}
	reference.scan(t, ctx)
	connection, model = connect()
	newInfo := waitName(model, renamed)
	final := apply(newInfo, &updated)
	store = open(false)
	catalog = mustCatalog(t, store, options)
	if !localEntry(t, catalog, name).Info.Deleted {
		t.Fatal("real-daemon tombstone was not durably adopted")
	}
	refs, err := store.Refs()
	if err != nil || len(refs) != 1 {
		t.Fatalf("unowned manifest pins: %v %v", refs, err)
	}
	data, err := store.ReadRange(final.Manifest, 0, uint64(len(after)))
	if err != nil || !bytes.Equal(data, after) {
		t.Fatal("reconnected catalog data differs", err)
	}
	if err := store.VerifyFile(original.Manifest); err != nil {
		t.Fatal("original history lost", err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	t.Log("real daemon: persisted original/edit/rename/tombstone, closed Stores during network waits, resumed indexes after offline rename, reused blocks without per-file pins")
}
