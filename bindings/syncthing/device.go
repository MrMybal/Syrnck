package syncthing

import (
	"bytes"
	"context"
	"crypto"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"time"
	"unicode/utf8"

	"github.com/syncthing/syncthing/lib/protocol"
)

// DeviceOptions binds a connection to an explicitly paired certificate identity.
// Certificate must contain one leaf and its matching private signing key. The
// caller must not mutate that key while the connection is alive. No CA, DNS name,
// remote Hello field or discovery response can grant device authorization.
type DeviceOptions struct {
	Certificate      tls.Certificate
	RemoteDevice     protocol.DeviceID
	Server           bool
	Hello            protocol.Hello
	Compression      protocol.Compression
	HandshakeTimeout time.Duration // zero selects 20 seconds; range 100ms..1min
}

// DeviceConnection is authenticated but not started. Call Start, then send the
// first ClusterConfig before Index/Request messages. The application model owns
// folder authorization, version vectors and durable index reconciliation.
// The context passed to OpenDeviceConnection controls the entire connection.
type DeviceConnection struct {
	protocol.Connection
	LocalDevice protocol.DeviceID
	PeerHello   protocol.Hello // untrusted descriptive fields, never credentials
	transport   net.Conn
}

// Close releases the transport even when Start was never called. Model teardown
// is asynchronous; wait for the model's Closed callback before closing its Store.
func (c *DeviceConnection) Close(reason error) {
	if reason == nil {
		reason = errors.New("syrnck BEP: connection closed")
	}
	_ = c.transport.Close()
	// The reference connection's graceful close waits for a writer. Starting
	// against the already closed stream avoids that wait for unstarted sessions.
	c.Connection.Start()
	c.Connection.Close(reason)
}

func pairedDevice(id protocol.DeviceID) bool {
	return id != protocol.EmptyDeviceID && id != protocol.LocalDeviceID && id != protocol.GlobalDeviceID
}

func signingPublic(signer crypto.Signer) (encoded []byte, err error) {
	// A malformed standard key (including a typed nil) can panic in Public or
	// its encoding. Treat it as invalid local configuration, before any network IO.
	defer func() {
		if recover() != nil {
			encoded = nil
			err = errors.New("syrnck BEP: invalid local signing key")
		}
	}()
	return x509.MarshalPKIXPublicKey(signer.Public())
}

func validateDevice(options *DeviceOptions) (protocol.DeviceID, error) {
	if len(options.Certificate.Certificate) != 1 || !pairedDevice(options.RemoteDevice) {
		return protocol.DeviceID{}, errors.New("syrnck BEP: explicit device identity and one local certificate required")
	}
	leaf, err := x509.ParseCertificate(options.Certificate.Certificate[0])
	if err != nil {
		return protocol.DeviceID{}, errors.New("syrnck BEP: invalid local device certificate")
	}
	signer, ok := options.Certificate.PrivateKey.(crypto.Signer)
	if !ok || signer == nil {
		return protocol.DeviceID{}, errors.New("syrnck BEP: local device signing key required")
	}
	public, err := signingPublic(signer)
	if err != nil {
		return protocol.DeviceID{}, errors.New("syrnck BEP: invalid local signing key")
	}
	if !bytes.Equal(public, leaf.RawSubjectPublicKeyInfo) {
		return protocol.DeviceID{}, errors.New("syrnck BEP: device certificate and signing key differ")
	}
	local := protocol.NewDeviceID(leaf.Raw)
	if !pairedDevice(local) || local == options.RemoteDevice {
		return protocol.DeviceID{}, errors.New("syrnck BEP: cannot connect a device to itself")
	}
	if options.Compression != protocol.CompressionMetadata && options.Compression != protocol.CompressionNever && options.Compression != protocol.CompressionAlways {
		return protocol.DeviceID{}, errors.New("syrnck BEP: invalid compression mode")
	}
	if options.HandshakeTimeout == 0 {
		options.HandshakeTimeout = 20 * time.Second
	}
	if options.HandshakeTimeout < 100*time.Millisecond || options.HandshakeTimeout > time.Minute {
		return protocol.DeviceID{}, errors.New("syrnck BEP: handshake timeout must be in 100ms..1min")
	}
	for _, field := range []string{options.Hello.DeviceName, options.Hello.ClientName, options.Hello.ClientVersion} {
		if !utf8.ValidString(field) || len(field) > 1024 {
			return protocol.DeviceID{}, errors.New("syrnck BEP: invalid local hello")
		}
	}
	if options.Hello.ClientName == "" {
		options.Hello.ClientName = "syrnck"
	}
	if options.Hello.ClientVersion == "" {
		options.Hello.ClientVersion = "0.1.0"
	}
	if options.Hello.Timestamp == 0 {
		options.Hello.Timestamp = time.Now().UnixNano()
	}
	if options.Hello.NumConnections == 0 {
		options.Hello.NumConnections = 1
	}
	if options.Hello.Timestamp < 0 || options.Hello.NumConnections < 1 || options.Hello.NumConnections > 64 {
		return protocol.DeviceID{}, errors.New("syrnck BEP: invalid local hello")
	}
	return local, nil
}

// OpenDeviceConnection takes ownership of transport, including on failure. It
// negotiates TLS 1.3+, exchanges the bounded upstream Hello and verifies the
// exact peer certificate's Syncthing Device ID before constructing a BEP model
// connection. Self-signed device certificates are supported through explicit
// pinning; ordinary Web PKI or certificate expiry does not define membership.
// Transport must be an unsecured connected stream. No plaintext fallback exists.
func OpenDeviceConnection(ctx context.Context, transport net.Conn, options DeviceOptions, model protocol.Model) (_ *DeviceConnection, err error) {
	if transport == nil {
		return nil, errors.New("syrnck BEP: connected transport required")
	}
	accepted := false
	defer func() {
		if !accepted {
			_ = transport.Close()
		}
	}()
	if ctx == nil || model == nil {
		return nil, errors.New("syrnck BEP: context and model required")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	local, err := validateDevice(&options)
	if err != nil {
		return nil, err
	}
	certificate := options.Certificate
	certificate.Certificate = [][]byte{bytes.Clone(certificate.Certificate[0])}
	certificate.Leaf = nil
	config := &tls.Config{MinVersion: tls.VersionTLS13, Certificates: []tls.Certificate{certificate},
		NextProtos: []string{"bep/1.0"}, ClientAuth: tls.RequestClientCert,
		// Device membership is checked by the exact SHA-256 certificate pin
		// below, after the mandatory Hello. It never depends on a CA or host.
		InsecureSkipVerify: true}
	secured := tls.Client(transport, config)
	if options.Server {
		secured = tls.Server(transport, config)
	}
	deadline := time.Now().Add(options.HandshakeTimeout)
	if limit, ok := ctx.Deadline(); ok && limit.Before(deadline) {
		deadline = limit
	}
	if err := secured.SetDeadline(deadline); err != nil {
		return nil, err
	}
	stop := context.AfterFunc(ctx, func() { _ = transport.Close() })
	defer stop()
	if err := secured.HandshakeContext(ctx); err != nil {
		return nil, fmt.Errorf("syrnck BEP: TLS handshake: %w", err)
	}
	remoteHello, err := protocol.ExchangeHello(secured, options.Hello)
	if err != nil {
		if ctx.Err() != nil {
			return nil, ctx.Err()
		}
		return nil, fmt.Errorf("syrnck BEP: hello exchange: %w", err)
	}
	state := secured.ConnectionState()
	if state.NegotiatedProtocol != "bep/1.0" || len(state.PeerCertificates) != 1 ||
		protocol.NewDeviceID(state.PeerCertificates[0].Raw) != options.RemoteDevice {
		return nil, errors.New("syrnck BEP: peer device identity or negotiated protocol does not match")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if err := secured.SetDeadline(time.Time{}); err != nil {
		return nil, err
	}
	var id [16]byte
	if _, err := rand.Read(id[:]); err != nil {
		return nil, err
	}
	info := deviceConnectionInfo{secured, time.Now(), hex.EncodeToString(id[:]), options.Server}
	connection := protocol.NewConnection(options.RemoteDevice, secured, secured, secured, model, info,
		options.Compression, protocol.NewKeyGenerator())
	result := &DeviceConnection{Connection: connection, LocalDevice: local, PeerHello: remoteHello, transport: transport}
	go func() {
		select {
		case <-ctx.Done():
			result.Close(ctx.Err())
		case <-connection.Closed():
		}
	}()
	accepted = true
	return result, nil
}

type deviceConnectionInfo struct {
	connection  *tls.Conn
	established time.Time
	id          string
	server      bool
}

func (i deviceConnectionInfo) Type() string {
	if i.server {
		return "syrnck-bep-server"
	}
	return "syrnck-bep-client"
}
func (i deviceConnectionInfo) Transport() string { return i.connection.RemoteAddr().Network() }
func (i deviceConnectionInfo) IsLocal() bool {
	address, ok := i.connection.RemoteAddr().(*net.TCPAddr)
	return ok && address.IP.IsLoopback()
}
func (i deviceConnectionInfo) RemoteAddr() net.Addr { return i.connection.RemoteAddr() }
func (i deviceConnectionInfo) Priority() int        { return 0 }
func (i deviceConnectionInfo) String() string {
	return i.Type() + " " + i.connection.RemoteAddr().String()
}
func (i deviceConnectionInfo) Crypto() string {
	return tls.VersionName(i.connection.ConnectionState().Version)
}
func (i deviceConnectionInfo) EstablishedAt() time.Time { return i.established }
func (i deviceConnectionInfo) ConnectionID() string     { return i.id }
