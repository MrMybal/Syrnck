package syncthing

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

func waitServiceCatalog(t *testing.T, ctx context.Context, folder *Folder, predicate func(FolderSnapshot) bool) FolderSnapshot {
	t.Helper()
	for {
		snapshot, err := folder.Snapshot(ctx)
		if err == nil && predicate(snapshot) {
			return snapshot
		}
		select {
		case <-ctx.Done():
			t.Fatal("resident catalog did not advance", ctx.Err(), err)
		case <-time.After(20 * time.Millisecond):
		}
	}
}

func TestFolderServiceOfflineCaptureReconnectAndShutdown(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
			defer cancel()
			leftCert, rightCert := deviceCertificate(t), deviceCertificate(t)
			leftID, rightID := protocol.NewDeviceID(leftCert.Certificate[0]), protocol.NewDeviceID(rightCert.Certificate[0])
			root := tempDirectory(t)
			suffix := "plain"
			if encrypted {
				suffix = "encrypted"
			}
			name := "Content/map.bin"
			before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
			offline := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
			left := createTestFolder(t, folderTestOptions(filepath.Join(root, "left-"+suffix), leftID, rightID, encrypted), map[string][]byte{name: before})
			right := createTestFolder(t, folderTestOptions(filepath.Join(root, "right-"+suffix), rightID, leftID, encrypted), nil)
			first := folderSnapshot(t, left)
			listener, err := net.ListenTCP("tcp", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			var online atomic.Bool
			var connections sync.Map
			session := FolderSessionOptions{Device: DeviceOptions{Certificate: leftCert, RemoteDevice: rightID}, PollInterval: 50 * time.Millisecond}
			options := FolderServiceOptions{Session: session, RetryMin: 50 * time.Millisecond, RetryMax: 200 * time.Millisecond, OfflineInterval: 50 * time.Millisecond,
				Connect: func(ctx context.Context) (net.Conn, error) {
					if !online.Load() {
						return nil, errors.New("test link offline")
					}
					stream, err := (&net.Dialer{Timeout: time.Second}).DialContext(ctx, "tcp", listener.Addr().String())
					if stream != nil {
						connections.Store(stream, true)
					}
					return stream, err
				}}
			leftService, err := OpenFolderService(ctx, left, options)
			if err != nil {
				t.Fatal(err)
			}
			defer leftService.Close()
			other := options
			other.Session.Device = DeviceOptions{Certificate: rightCert, RemoteDevice: leftID, Server: true}
			other.Connect = func(ctx context.Context) (net.Conn, error) {
				for {
					if err := ctx.Err(); err != nil {
						return nil, err
					}
					if err := listener.SetDeadline(time.Now().Add(50 * time.Millisecond)); err != nil {
						return nil, err
					}
					stream, err := listener.Accept()
					if timeout, ok := err.(net.Error); ok && timeout.Timeout() {
						continue
					}
					if stream != nil {
						connections.Store(stream, true)
					}
					return stream, err
				}
			}
			rightService, err := OpenFolderService(ctx, right, other)
			if err != nil {
				t.Fatal(err)
			}
			defer rightService.Close()
			// A blocked accept/dial never prevents offline capture or holds Store.
			if err := os.WriteFile(filepath.Join(left.options.Directory, filepath.FromSlash(name)), offline, 0600); err != nil {
				t.Fatal(err)
			}
			waitServiceCatalog(t, ctx, left, func(snapshot FolderSnapshot) bool {
				return snapshot.Catalog.ID != first.Catalog.ID && len(snapshot.Files) == 1 && snapshot.Files[0].Manifest != first.Files[0].Manifest
			})
			online.Store(true)
			waitFolderApplication(t, ctx, right, name, offline)
			// Let the local owner acknowledge receipt before an explicit offline rename.
			online.Store(false)
			connections.Range(func(conn, value any) bool { _ = conn.(net.Conn).Close(); connections.Delete(conn); return true })
			renamed := "Content/renamed.bin"
			if err := os.Rename(filepath.Join(left.options.Directory, filepath.FromSlash(name)), filepath.Join(left.options.Directory, filepath.FromSlash(renamed))); err != nil {
				t.Fatal(err)
			}
			waitServiceCatalog(t, ctx, left, func(snapshot FolderSnapshot) bool {
				deleted, created := false, false
				for _, entry := range snapshot.Files {
					deleted = deleted || (entry.Info.Name == name && entry.Info.Deleted)
					created = created || (entry.Info.Name == renamed && !entry.Info.Deleted && entry.Manifest != "")
				}
				return deleted && created
			})
			online.Store(true)
			waitFolderApplication(t, ctx, right, renamed, offline)
			for {
				_, err := os.Stat(filepath.Join(right.options.Directory, filepath.FromSlash(name)))
				if os.IsNotExist(err) {
					break
				}
				select {
				case <-ctx.Done():
					t.Fatal("offline deletion did not converge", err)
				case <-time.After(20 * time.Millisecond):
				}
			}
			if err := leftService.Close(); err != nil {
				t.Fatal(err)
			}
			if err := rightService.Close(); err != nil {
				t.Fatal(err)
			}
			select {
			case <-leftService.Done():
			default:
				t.Fatal("service returned before workers stopped")
			}
			// Store/key ownership is independent of resident service lifetime.
			store, err := left.options.OpenStore(ctx)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := store.Refs(); err != nil {
				t.Fatal(err)
			}
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestFolderServiceCancellationJoinsTransportFactory(t *testing.T) {
	cert, peer := deviceCertificate(t), deviceCertificate(t)
	local, remote := protocol.NewDeviceID(cert.Certificate[0]), protocol.NewDeviceID(peer.Certificate[0])
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), local, remote, false), nil)
	started, finished := make(chan struct{}), make(chan struct{})
	options := FolderServiceOptions{Session: FolderSessionOptions{Device: DeviceOptions{Certificate: cert, RemoteDevice: remote}},
		Connect: func(ctx context.Context) (net.Conn, error) {
			close(started)
			<-ctx.Done()
			close(finished)
			return nil, ctx.Err()
		}}
	ctx, cancel := context.WithCancel(context.Background())
	service, err := OpenFolderService(ctx, folder, options)
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("factory not started")
	}
	cancel()
	if err := service.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-finished:
	default:
		t.Fatal("factory leaked after Close")
	}
	for range service.Events() {
	} // All advisory channels close after cleanup.
	for range service.Reports() {
	}
	for range service.Errors() {
	}
}

func TestFolderServiceClosesFailedFactoryTransportAndValidatesBeforeIO(t *testing.T) {
	cert, peer := deviceCertificate(t), deviceCertificate(t)
	local, remote := protocol.NewDeviceID(cert.Certificate[0]), protocol.NewDeviceID(peer.Certificate[0])
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), local, remote, false), nil)
	stream, reader := net.Pipe()
	defer reader.Close()
	var calls atomic.Int32
	options := FolderServiceOptions{Session: FolderSessionOptions{Device: DeviceOptions{Certificate: cert, RemoteDevice: remote}},
		Connect: func(context.Context) (net.Conn, error) {
			calls.Add(1)
			return stream, errors.New("test failed after opening")
		}}
	bad := options
	bad.RetryMin = -1
	if _, err := OpenFolderService(context.Background(), folder, bad); err == nil || calls.Load() != 0 {
		t.Fatal("invalid service reached transport", err)
	}
	// Set the probe deadline before the worker can close its failed transport.
	// net.Pipe refuses deadline changes after either endpoint has closed.
	if err := reader.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	service, err := OpenFolderService(context.Background(), folder, options)
	if err != nil {
		t.Fatal(err)
	}
	defer service.Close()
	var b [1]byte
	if _, err := reader.Read(b[:]); !errors.Is(err, io.EOF) {
		t.Fatal("failed factory transport leaked", err)
	}
}

func TestFolderServiceCancellationJoinsStalledTLS(t *testing.T) {
	cert, peer := deviceCertificate(t), deviceCertificate(t)
	local, remote := protocol.NewDeviceID(cert.Certificate[0]), protocol.NewDeviceID(peer.Certificate[0])
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), local, remote, false), nil)
	stream, reader := net.Pipe()
	defer reader.Close()
	options := FolderServiceOptions{Session: FolderSessionOptions{Device: DeviceOptions{Certificate: cert, RemoteDevice: remote}},
		Connect: func(context.Context) (net.Conn, error) { return stream, nil }}
	service, err := OpenFolderService(context.Background(), folder, options)
	if err != nil {
		t.Fatal(err)
	}
	defer service.Close()
	if err := reader.SetReadDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatal(err)
	}
	var first [1]byte
	// Consume only one byte of ClientHello: its writer remains blocked inside
	// the real TLS handshake until service cancellation closes the transport.
	if _, err := reader.Read(first[:]); err != nil {
		t.Fatal("TLS handshake never started", err)
	}
	stopped := make(chan struct{})
	go func() { _ = service.Close(); close(stopped) }()
	select {
	case <-stopped:
	case <-time.After(2 * time.Second):
		t.Fatal("stalled TLS worker leaked after Close")
	}
}

func TestFolderServiceCapturesWhileAuthenticatedPeerWithholdsConfiguration(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cert, peer := deviceCertificate(t), deviceCertificate(t)
	local, remote := protocol.NewDeviceID(cert.Certificate[0]), protocol.NewDeviceID(peer.Certificate[0])
	folder := createTestFolder(t, folderTestOptions(filepath.Join(tempDirectory(t), "plain"), local, remote, false), nil)
	stream, other := streams(t)
	var used atomic.Bool
	options := FolderServiceOptions{Session: FolderSessionOptions{Device: DeviceOptions{Certificate: cert, RemoteDevice: remote}}, OfflineInterval: 50 * time.Millisecond,
		Connect: func(ctx context.Context) (net.Conn, error) {
			if !used.Swap(true) {
				return stream, nil
			}
			<-ctx.Done()
			return nil, ctx.Err()
		}}
	service, err := OpenFolderService(ctx, folder, options)
	if err != nil {
		t.Fatal(err)
	}
	defer service.Close()
	model := newWireModel(nil)
	connection, err := OpenDeviceConnection(ctx, other, DeviceOptions{Certificate: peer, RemoteDevice: local, Server: true}, model)
	if err != nil {
		t.Fatal(err)
	}
	connection.Start()
	defer func() { connection.Close(nil); modelClosed(t, model) }()
	select {
	case <-model.configured:
	case <-ctx.Done():
		t.Fatal("authenticated peer did not receive local configuration")
	}
	// The peer deliberately never sends ClusterConfig. This must not stall
	// snapshots of local work behind protocol negotiation.
	path := filepath.Join(folder.options.Directory, "local.cpp")
	if err := os.WriteFile(path, []byte("captured without peer progress"), 0600); err != nil {
		t.Fatal(err)
	}
	waitServiceCatalog(t, ctx, folder, func(snapshot FolderSnapshot) bool {
		return len(snapshot.Files) == 1 && snapshot.Files[0].Info.Name == "local.cpp" && snapshot.Files[0].Manifest != ""
	})
}
