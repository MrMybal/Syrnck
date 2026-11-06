package syncthing

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

type wireModel struct {
	file       *File
	index      chan protocol.FileInfo
	configured chan struct{}
	closed     chan error
	once       sync.Once
}

func newWireModel(file *File) *wireModel {
	return &wireModel{file: file, index: make(chan protocol.FileInfo, 1), configured: make(chan struct{}), closed: make(chan error, 1)}
}
func (m *wireModel) Index(_ protocol.Connection, idx *protocol.Index) error {
	if len(idx.Files) == 1 {
		m.index <- idx.Files[0]
	}
	return nil
}
func (*wireModel) IndexUpdate(protocol.Connection, *protocol.IndexUpdate) error { return nil }
func (m *wireModel) ClusterConfig(protocol.Connection, *protocol.ClusterConfig) error {
	m.once.Do(func() { close(m.configured) })
	return nil
}
func (m *wireModel) Closed(_ protocol.Connection, err error) {
	select {
	case m.closed <- err:
	default:
	}
}
func (*wireModel) DownloadProgress(protocol.Connection, *protocol.DownloadProgress) error { return nil }
func (m *wireModel) Request(_ protocol.Connection, req *protocol.Request) (protocol.RequestResponse, error) {
	if m.file == nil {
		return nil, protocol.ErrNoSuchFile
	}
	data, err := m.file.ReadRequest(context.Background(), req)
	if err != nil {
		return nil, err
	}
	return &wireResponse{data: data, done: make(chan struct{})}, nil
}

type wireResponse struct {
	data []byte
	done chan struct{}
	once sync.Once
}

func (r *wireResponse) Data() []byte { return r.data }
func (r *wireResponse) Close()       { r.once.Do(func() { close(r.done) }) }
func (r *wireResponse) Wait()        { <-r.done }

type wireInfo struct {
	conn net.Conn
	id   string
}

func (i wireInfo) Type() string             { return "test" }
func (i wireInfo) Transport() string        { return "tcp" }
func (i wireInfo) IsLocal() bool            { return true }
func (i wireInfo) RemoteAddr() net.Addr     { return i.conn.RemoteAddr() }
func (i wireInfo) Priority() int            { return 1 }
func (i wireInfo) String() string           { return i.id }
func (i wireInfo) Crypto() string           { return "TLS1.3" }
func (i wireInfo) EstablishedAt() time.Time { return time.Now() }
func (i wireInfo) ConnectionID() string     { return i.id }

func certificates(t *testing.T) (*x509.CertPool, tls.Certificate, tls.Certificate) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	ca := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "Syrnck BEP test CA"},
		NotBefore: time.Now().Add(-time.Minute), NotAfter: time.Now().Add(time.Hour), IsCA: true, BasicConstraintsValid: true,
		KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature}
	der, err := x509.CreateCertificate(rand.Reader, ca, ca, public, private)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	pool := x509.NewCertPool()
	pool.AddCert(parsed)
	leaf := func(serial int64) tls.Certificate {
		pub, priv, err := ed25519.GenerateKey(rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		certificate := &x509.Certificate{SerialNumber: big.NewInt(serial), DNSNames: []string{"localhost"},
			NotBefore: ca.NotBefore, NotAfter: ca.NotAfter, KeyUsage: x509.KeyUsageDigitalSignature,
			ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth, x509.ExtKeyUsageClientAuth}}
		encoded, err := x509.CreateCertificate(rand.Reader, certificate, parsed, pub, private)
		if err != nil {
			t.Fatal(err)
		}
		return tls.Certificate{Certificate: [][]byte{encoded, der}, PrivateKey: priv}
	}
	return pool, leaf(2), leaf(3)
}

func TestReferenceBEPConnectionOverMutualTLS13(t *testing.T) {
	for _, compression := range []protocol.Compression{protocol.CompressionNever, protocol.CompressionAlways} {
		t.Run(map[protocol.Compression]string{protocol.CompressionNever: "raw", protocol.CompressionAlways: "compressed"}[compression], func(t *testing.T) {
			source, destination := openStore(t, true), openStore(t, true)
			before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
			after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
			reuse := ingest(t, destination, before, true)
			incoming := file(t, source, ingest(t, source, after, false))
			pool, clientCertificate, serverCertificate := certificates(t)
			listener, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			accepted := make(chan net.Conn, 1)
			go func() { connection, _ := listener.Accept(); accepted <- connection }()
			left, err := net.DialTimeout("tcp", listener.Addr().String(), 2*time.Second)
			if err != nil {
				t.Fatal(err)
			}
			right := <-accepted
			if right == nil {
				t.Fatal("loopback accept failed")
			}
			t.Cleanup(func() { _ = left.Close(); _ = right.Close() })
			_ = left.SetDeadline(time.Now().Add(15 * time.Second))
			_ = right.SetDeadline(time.Now().Add(15 * time.Second))
			clientTLS := tls.Client(left, &tls.Config{MinVersion: tls.VersionTLS13, RootCAs: pool,
				Certificates: []tls.Certificate{clientCertificate}, ServerName: "localhost", NextProtos: []string{"bep/1.0"}})
			serverTLS := tls.Server(right, &tls.Config{MinVersion: tls.VersionTLS13, ClientCAs: pool,
				Certificates: []tls.Certificate{serverCertificate}, ClientAuth: tls.RequireAndVerifyClientCert, NextProtos: []string{"bep/1.0"}})
			ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
			defer cancel()
			serverReady := make(chan error, 1)
			go func() {
				if err := serverTLS.HandshakeContext(ctx); err != nil {
					serverReady <- err
					return
				}
				_, err := protocol.ExchangeHello(serverTLS, protocol.Hello{DeviceName: "source", ClientName: "syrnck-test", ClientVersion: "0.1", Timestamp: time.Now().UnixNano()})
				serverReady <- err
			}()
			if err := clientTLS.HandshakeContext(ctx); err != nil {
				t.Fatal(err)
			}
			if _, err := protocol.ExchangeHello(clientTLS, protocol.Hello{DeviceName: "destination", ClientName: "syrnck-test", ClientVersion: "0.1", Timestamp: time.Now().UnixNano()}); err != nil {
				t.Fatal(err)
			}
			if err := <-serverReady; err != nil {
				t.Fatal(err)
			}
			if clientTLS.ConnectionState().Version != tls.VersionTLS13 || len(serverTLS.ConnectionState().VerifiedChains) == 0 {
				t.Fatal("mutual TLS 1.3 was not verified")
			}
			clientModel, serverModel := newWireModel(nil), newWireModel(incoming)
			client := protocol.NewConnection(protocol.NewDeviceID(serverCertificate.Certificate[0]), clientTLS, clientTLS, clientTLS,
				clientModel, wireInfo{clientTLS, "destination"}, compression, protocol.NewKeyGenerator())
			server := protocol.NewConnection(protocol.NewDeviceID(clientCertificate.Certificate[0]), serverTLS, serverTLS, serverTLS,
				serverModel, wireInfo{serverTLS, "source"}, compression, protocol.NewKeyGenerator())
			t.Cleanup(func() {
				_ = left.Close()
				_ = right.Close()
				client.Close(errors.New("test complete"))
				server.Close(errors.New("test complete"))
				// Close starts asynchronous teardown. The model notification follows
				// dispatcher shutdown; wait before Store cleanup closes native handles.
				for _, model := range []*wireModel{clientModel, serverModel} {
					select {
					case <-model.closed:
					case <-time.After(2 * time.Second):
						t.Error("BEP dispatcher did not shut down")
					}
				}
			})
			client.Start()
			server.Start()
			client.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project"}}}, nil)
			server.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project"}}}, nil)
			for _, model := range []*wireModel{clientModel, serverModel} {
				select {
				case <-model.configured:
				case err := <-model.closed:
					t.Fatal(err)
				case <-ctx.Done():
					t.Fatal(ctx.Err())
				}
			}
			if err := server.Index(ctx, &protocol.Index{Folder: "project", Files: []protocol.FileInfo{incoming.Info()}}); err != nil {
				t.Fatal(err)
			}
			var index protocol.FileInfo
			select {
			case index = <-clientModel.index:
			case err := <-clientModel.closed:
				t.Fatal(err)
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			result, err := Fetch(ctx, destination, "project", index, client.Request, FetchOptions{ReuseManifest: reuse})
			if err != nil {
				t.Fatal(err)
			}
			if result.Stats.Requests != 1 || result.Stats.TransferredBytes != protocol.MinBlockSize {
				t.Fatalf("wire sent unchanged blocks: %+v", result.Stats)
			}
			output := filepath.Join(tempDirectory(t), "received")
			if err := destination.Restore(result.Manifest, output); err != nil {
				t.Fatal(err)
			}
			got, err := os.ReadFile(output)
			if err != nil || !bytes.Equal(got, after) {
				t.Fatal("wire reconstruction differs", err)
			}
		})
	}
}
