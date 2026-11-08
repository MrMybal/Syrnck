package syncthing

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"io"
	"math/big"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

func deviceCertificate(t *testing.T) tls.Certificate {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	certificate := &x509.Certificate{SerialNumber: big.NewInt(1),
		Subject: pkix.Name{CommonName: "syncthing"}, DNSNames: []string{"syncthing"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth, x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, certificate, certificate, public, private)
	if err != nil {
		t.Fatal(err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: private}
}

func streams(t *testing.T) (net.Conn, net.Conn) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	accepted := make(chan net.Conn, 1)
	go func() { stream, _ := listener.Accept(); accepted <- stream }()
	left, err := net.DialTimeout("tcp", listener.Addr().String(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	right := <-accepted
	if right == nil {
		t.Fatal("loopback accept failed")
	}
	t.Cleanup(func() { _ = left.Close(); _ = right.Close() })
	return left, right
}

func modelClosed(t *testing.T, model *wireModel) {
	t.Helper()
	select {
	case <-model.closed:
	case <-time.After(2 * time.Second):
		t.Fatal("authenticated model did not stop")
	}
}

func TestPinnedDeviceBlockTransferAndLifetime(t *testing.T) {
	for _, compression := range []protocol.Compression{protocol.CompressionNever, protocol.CompressionAlways} {
		t.Run(map[protocol.Compression]string{protocol.CompressionNever: "raw", protocol.CompressionAlways: "compressed"}[compression], func(t *testing.T) {
			source, destination := openStore(t, true), openStore(t, true)
			before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
			after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
			reuse := ingest(t, destination, before, true)
			file := file(t, source, ingest(t, source, after, false))
			leftCert, rightCert := deviceCertificate(t), deviceCertificate(t)
			leftID, rightID := protocol.NewDeviceID(leftCert.Certificate[0]), protocol.NewDeviceID(rightCert.Certificate[0])
			left, right := streams(t)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			leftModel, rightModel := newWireModel(nil), newWireModel(file)
			serverReady := make(chan *DeviceConnection, 1)
			serverError := make(chan error, 1)
			go func() {
				connection, err := OpenDeviceConnection(ctx, right, DeviceOptions{Certificate: rightCert, RemoteDevice: leftID,
					Server: true, Compression: compression, Hello: protocol.Hello{DeviceName: "source"}}, rightModel)
				if err != nil {
					serverError <- err
				} else {
					serverReady <- connection
				}
			}()
			client, err := OpenDeviceConnection(ctx, left, DeviceOptions{Certificate: leftCert, RemoteDevice: rightID,
				Compression: compression, Hello: protocol.Hello{DeviceName: "receiver"}}, leftModel)
			if err != nil {
				t.Fatal(err)
			}
			var server *DeviceConnection
			select {
			case server = <-serverReady:
			case err = <-serverError:
				t.Fatal(err)
			case <-time.After(3 * time.Second):
				t.Fatal("device handshake stalled")
			}
			defer func() { client.Close(nil); server.Close(nil) }()
			if client.DeviceID() != rightID || server.DeviceID() != leftID || client.LocalDevice != leftID ||
				client.PeerHello.DeviceName != "source" || client.PeerHello.ClientName != "syrnck" ||
				client.Crypto() != "TLS 1.3" || !client.IsLocal() || client.EstablishedAt() != client.EstablishedAt() || client.ConnectionID() == "" {
				t.Fatal("device authentication or stable connection metadata differs")
			}
			client.Start()
			server.Start()
			client.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project"}}}, nil)
			server.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project"}}}, nil)
			for _, model := range []*wireModel{leftModel, rightModel} {
				select {
				case <-model.configured:
				case <-time.After(2 * time.Second):
					t.Fatal("cluster configuration stalled")
				}
			}
			result, err := Fetch(ctx, destination, "project", file.Info(), client.Request, FetchOptions{ReuseManifest: reuse})
			if err != nil {
				t.Fatal(err)
			}
			if result.Stats.Requests != 1 || result.Stats.ReusedBytes != 2*uint64(protocol.MinBlockSize) {
				t.Fatalf("authenticated differential transfer: %+v", result.Stats)
			}
			cancel()
			modelClosed(t, leftModel)
			modelClosed(t, rightModel)
		})
	}
}

func TestUnpairedDeviceGetsNoModelConnection(t *testing.T) {
	leftCert, rightCert, unknown := deviceCertificate(t), deviceCertificate(t), deviceCertificate(t)
	left, right := streams(t)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	model := newWireModel(nil)
	type result struct {
		connection *DeviceConnection
		err        error
	}
	ready := make(chan result, 1)
	go func() {
		connection, err := OpenDeviceConnection(ctx, right, DeviceOptions{Certificate: rightCert,
			RemoteDevice: protocol.NewDeviceID(leftCert.Certificate[0]), Server: true}, newWireModel(nil))
		ready <- result{connection, err}
	}()
	connection, err := OpenDeviceConnection(ctx, left, DeviceOptions{Certificate: leftCert,
		RemoteDevice: protocol.NewDeviceID(unknown.Certificate[0])}, model)
	if connection != nil || err == nil || !strings.Contains(err.Error(), "peer device identity") {
		t.Fatal("unpaired device authenticated")
	}
	select {
	case <-model.configured:
		t.Fatal("unauthorized cluster callback")
	case <-model.index:
		t.Fatal("unauthorized index callback")
	default:
	}
	server := <-ready
	if server.err != nil || server.connection == nil {
		t.Fatalf("paired server handshake: %v", server.err)
	}
	server.connection.Close(nil)
}

func TestDeviceIdentityValidationAndCancellation(t *testing.T) {
	certificate, other := deviceCertificate(t), deviceCertificate(t)
	valid := DeviceOptions{Certificate: certificate, RemoteDevice: protocol.NewDeviceID(other.Certificate[0])}
	for _, change := range []func(*DeviceOptions){
		func(o *DeviceOptions) { o.RemoteDevice = protocol.EmptyDeviceID },
		func(o *DeviceOptions) { o.RemoteDevice = protocol.LocalDeviceID },
		func(o *DeviceOptions) { o.RemoteDevice = protocol.GlobalDeviceID },
		func(o *DeviceOptions) { o.RemoteDevice = protocol.NewDeviceID(certificate.Certificate[0]) },
		func(o *DeviceOptions) { o.Certificate.PrivateKey = other.PrivateKey },
		func(o *DeviceOptions) { o.Certificate.PrivateKey = ed25519.PrivateKey(nil) },
		func(o *DeviceOptions) { o.Certificate.PrivateKey = (*rsa.PrivateKey)(nil) },
		func(o *DeviceOptions) { o.Certificate.Certificate = [][]byte{[]byte("invalid")} },
		func(o *DeviceOptions) { o.HandshakeTimeout = time.Nanosecond },
		func(o *DeviceOptions) { o.Hello.DeviceName = string(bytes.Repeat([]byte{'x'}, 1025)) },
	} {
		left, right := streams(t)
		options := valid
		change(&options)
		if connection, err := OpenDeviceConnection(context.Background(), left, options, newWireModel(nil)); connection != nil || err == nil {
			t.Fatal("invalid identity/options accepted")
		}
		_ = right.SetReadDeadline(time.Now().Add(time.Second))
		var data [1]byte
		if n, _ := right.Read(data[:]); n != 0 {
			t.Fatal("invalid identity wrote protocol bytes")
		}
	}
	left, _ := streams(t)
	ctx, cancel := context.WithCancel(context.Background())
	finished := make(chan error, 1)
	go func() { _, err := OpenDeviceConnection(ctx, left, valid, newWireModel(nil)); finished <- err }()
	cancel()
	select {
	case err := <-finished:
		if !errors.Is(err, context.Canceled) {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("TLS cancellation did not release the stream")
	}
}

func TestDeviceRejectsMissingALPNCertificateChainsAndOldTLS(t *testing.T) {
	for _, mode := range []string{"missing-alpn", "missing-certificate", "certificate-chain", "tls12"} {
		t.Run(mode, func(t *testing.T) {
			clientCert, serverCert := deviceCertificate(t), deviceCertificate(t)
			clientID := protocol.NewDeviceID(clientCert.Certificate[0])
			left, right := streams(t)
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			model := newWireModel(nil)
			ready := make(chan error, 1)
			go func() {
				connection, err := OpenDeviceConnection(ctx, right, DeviceOptions{Certificate: serverCert,
					RemoteDevice: clientID, Server: true}, model)
				if connection != nil {
					connection.Close(nil)
					ready <- errors.New("invalid peer authenticated")
					return
				}
				ready <- err
			}()
			config := &tls.Config{MinVersion: tls.VersionTLS13, InsecureSkipVerify: true,
				Certificates: []tls.Certificate{clientCert}, NextProtos: []string{"bep/1.0"}}
			switch mode {
			case "missing-alpn":
				config.NextProtos = nil
			case "missing-certificate":
				config.Certificates = nil
			case "certificate-chain":
				config.Certificates[0].Certificate = [][]byte{clientCert.Certificate[0], clientCert.Certificate[0]}
			case "tls12":
				config.MinVersion = tls.VersionTLS12
				config.MaxVersion = tls.VersionTLS12
			}
			peer := tls.Client(left, config)
			if err := peer.HandshakeContext(ctx); err == nil {
				if mode == "tls12" {
					t.Fatal("old TLS negotiated")
				}
				hello, err := protocol.ExchangeHello(peer, protocol.Hello{ClientName: "untrusted test", Timestamp: time.Now().UnixNano()})
				if err != nil || hello.ClientName != "syrnck" {
					t.Fatalf("mandatory pre-auth Hello was not sent: %v", err)
				}
			} else if mode != "tls12" {
				t.Fatal(err)
			}
			select {
			case err := <-ready:
				if err == nil {
					t.Fatal("invalid peer accepted")
				}
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			select {
			case <-model.configured:
				t.Fatal("unauthorized model call")
			case <-model.index:
				t.Fatal("unauthorized model call")
			default:
			}
		})
	}
}

func TestDeviceHelloCancellationAndUnstartedLifetime(t *testing.T) {
	clientCert, serverCert := deviceCertificate(t), deviceCertificate(t)
	left, right := streams(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	helloReady := make(chan error, 1)
	go func() {
		peer := tls.Server(right, &tls.Config{MinVersion: tls.VersionTLS13, Certificates: []tls.Certificate{serverCert},
			NextProtos: []string{"bep/1.0"}, ClientAuth: tls.RequestClientCert})
		if err := peer.HandshakeContext(ctx); err != nil {
			helloReady <- err
			return
		}
		var header [6]byte
		_, err := io.ReadFull(peer, header[:])
		helloReady <- err // Intentionally withhold the remote Hello.
	}()
	finished := make(chan error, 1)
	go func() {
		_, err := OpenDeviceConnection(ctx, left, DeviceOptions{Certificate: clientCert,
			RemoteDevice: protocol.NewDeviceID(serverCert.Certificate[0])}, newWireModel(nil))
		finished <- err
	}()
	select {
	case err := <-helloReady:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("TLS/Hello stalled")
	}
	cancel()
	select {
	case err := <-finished:
		if !errors.Is(err, context.Canceled) {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("Hello cancellation stalled")
	}

	left, right = streams(t)
	ctx, cancel = context.WithCancel(context.Background())
	defer cancel()
	leftModel, rightModel := newWireModel(nil), newWireModel(nil)
	serverReady := make(chan *DeviceConnection, 1)
	serverError := make(chan error, 1)
	go func() {
		connection, err := OpenDeviceConnection(ctx, right, DeviceOptions{Certificate: serverCert,
			RemoteDevice: protocol.NewDeviceID(clientCert.Certificate[0]), Server: true}, rightModel)
		if err != nil {
			serverError <- err
		} else {
			serverReady <- connection
		}
	}()
	client, err := OpenDeviceConnection(ctx, left, DeviceOptions{Certificate: clientCert,
		RemoteDevice: protocol.NewDeviceID(serverCert.Certificate[0])}, leftModel)
	if err != nil {
		t.Fatal(err)
	}
	var server *DeviceConnection
	select {
	case server = <-serverReady:
	case err := <-serverError:
		t.Fatal(err)
	case <-time.After(time.Second):
		t.Fatal("unstarted session stalled")
	}
	// Neither caller started BEP dispatch. Cancellation must still close both
	// sessions promptly, without waiting for the reference graceful-close timer.
	cancel()
	modelClosed(t, leftModel)
	modelClosed(t, rightModel)
	client.Close(nil)
	server.Close(nil)
}
