package syncthing

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"encoding/xml"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
)

// This suite deliberately uses the real unmodified upstream daemon and its
// scanner, SQLite database, model and puller, rather than a second test model.
// The optional binary is built separately from the exact module in go.mod.
func TestReferenceDaemonDifferentialTransfers(t *testing.T) {
	binary := os.Getenv("SYRNCK_SYNCTHING_BINARY")
	if binary == "" {
		t.Skip("set SYRNCK_SYNCTHING_BINARY to qualify the real upstream daemon")
	}
	if !filepath.IsAbs(binary) {
		t.Fatal("SYRNCK_SYNCTHING_BINARY must be absolute")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	source, destination := openStore(t, true), openStore(t, true)
	name := "Content/Map.uasset"
	before := bytes.Join([][]byte{block(1), block(2), block(3)}, nil)
	after := bytes.Join([][]byte{block(3), block(1), block(4)}, nil)
	localCert := deviceCertificate(t)
	localID := protocol.NewDeviceID(localCert.Certificate[0])
	reference := startReference(t, ctx, binary, localID, name, before)
	stream, err := net.DialTimeout("tcp", reference.bep, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	view, err := NewFile(ctx, source, ingest(t, source, after, false), "project", name, protocol.MinBlockSize)
	if err != nil {
		t.Fatal(err)
	}
	model := &daemonModel{view: view, configured: make(chan struct{}, 1), indexes: make(chan protocol.FileInfo, 256), closed: make(chan error, 1)}
	connection, err := OpenDeviceConnection(ctx, stream, DeviceOptions{Certificate: localCert, RemoteDevice: reference.device}, model)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		connection.Close(nil)
		select {
		case <-model.closed:
		case <-time.After(2 * time.Second):
			t.Error("daemon connection did not close")
		}
	})
	connection.Start()
	connection.ClusterConfig(&protocol.ClusterConfig{Folders: []protocol.Folder{{ID: "project", Type: protocol.FolderTypeSendReceive,
		Devices: []protocol.Device{{ID: localID, IndexID: 1}, {ID: reference.device}}}}}, nil)
	select {
	case <-model.configured:
	case err := <-model.closed:
		t.Fatal(err)
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if err := connection.Index(ctx, &protocol.Index{Folder: "project"}); err != nil {
		t.Fatal(err)
	}
	original := model.wait(t, ctx, func(info protocol.FileInfo) bool { return filepath.ToSlash(info.Name) == name && !info.Deleted })
	info := view.Info()
	info.Sequence = 1
	info.Version = original.Version.Update(localID.Short())
	info.ModifiedBy = localID.Short()
	info.ModifiedS = time.Now().Unix()
	info.ModifiedNs = 0
	if err := connection.IndexUpdate(ctx, &protocol.IndexUpdate{Folder: "project", Files: []protocol.FileInfo{info}}); err != nil {
		t.Fatal(err)
	}
	reference.waitFile(t, ctx, name, after)
	if model.requests.Load() != 1 || model.transferred.Load() != uint64(protocol.MinBlockSize) {
		t.Fatalf("daemon did not reuse moved local blocks: %d requests, %d bytes", model.requests.Load(), model.transferred.Load())
	}
	// A subsequent real-daemon edit must be announced with a dominating vector
	// and reconstructed into encrypted engine storage using one missing block.
	changed := bytes.Join([][]byte{block(3), block(5), block(4)}, nil)
	if err := os.WriteFile(filepath.Join(reference.folder, filepath.FromSlash(name)), changed, 0600); err != nil {
		t.Fatal(err)
	}
	modified := time.Now().Add(3 * time.Second)
	if err := os.Chtimes(filepath.Join(reference.folder, filepath.FromSlash(name)), modified, modified); err != nil {
		t.Fatal(err)
	}
	reference.scan(t, ctx)
	update := model.wait(t, ctx, func(incoming protocol.FileInfo) bool {
		return filepath.ToSlash(incoming.Name) == name && !incoming.Deleted && incoming.Version.GreaterEqual(info.Version) && !incoming.Version.Equal(info.Version)
	})
	reuse := ingest(t, destination, after, true)
	result, err := Fetch(ctx, destination, "project", update, connection.Request, FetchOptions{ReuseManifest: reuse})
	if err != nil {
		t.Fatal(err)
	}
	if result.Stats.Requests != 1 || result.Stats.ReusedBytes != 2*uint64(protocol.MinBlockSize) {
		t.Fatalf("daemon-to-engine reuse: %+v", result.Stats)
	}
	data, err := destination.ReadRange(result.Manifest, 0, uint64(len(changed)))
	if err != nil || !bytes.Equal(data, changed) {
		t.Fatalf("verified daemon reconstruction differs: %v", err)
	}
	// Exercise actual upstream rename/deletion announcements as well. They are
	// observations here; this low-level adapter never mutates a native workspace.
	renamed := "Content/Renamed.uasset"
	if err := os.Rename(filepath.Join(reference.folder, filepath.FromSlash(name)), filepath.Join(reference.folder, filepath.FromSlash(renamed))); err != nil {
		t.Fatal(err)
	}
	reference.scan(t, ctx)
	oldDeleted, newPresent := false, false
	for !oldDeleted || !newPresent {
		row := model.wait(t, ctx, func(incoming protocol.FileInfo) bool {
			path := filepath.ToSlash(incoming.Name)
			return path == name && incoming.Deleted || path == renamed && !incoming.Deleted
		})
		oldDeleted = oldDeleted || filepath.ToSlash(row.Name) == name && row.Deleted
		newPresent = newPresent || filepath.ToSlash(row.Name) == renamed && !row.Deleted
	}
	t.Logf("real daemon: engine→daemon %d missing bytes; daemon→engine %+v; rename tombstone and new path observed", model.transferred.Load(), result.Stats)
}

type daemonModel struct {
	view                  *File
	configured            chan struct{}
	indexes               chan protocol.FileInfo
	closed                chan error
	requests, transferred atomic.Uint64
}

func (m *daemonModel) Index(_ protocol.Connection, index *protocol.Index) error {
	return m.index(index.Folder, index.Files)
}
func (m *daemonModel) IndexUpdate(_ protocol.Connection, index *protocol.IndexUpdate) error {
	return m.index(index.Folder, index.Files)
}
func (m *daemonModel) index(folder string, files []protocol.FileInfo) error {
	if folder != "project" {
		return protocol.ErrNoSuchFile
	}
	for _, info := range files {
		select {
		case m.indexes <- copyInfo(info):
		default:
			return fmt.Errorf("test index capacity exceeded")
		}
	}
	return nil
}
func (m *daemonModel) ClusterConfig(_ protocol.Connection, config *protocol.ClusterConfig) error {
	for _, folder := range config.Folders {
		if folder.ID == "project" {
			select {
			case m.configured <- struct{}{}:
			default:
			}
		}
	}
	return nil
}
func (m *daemonModel) Closed(_ protocol.Connection, err error) {
	select {
	case m.closed <- err:
	default:
	}
}
func (*daemonModel) DownloadProgress(protocol.Connection, *protocol.DownloadProgress) error {
	return nil
}
func (m *daemonModel) Request(_ protocol.Connection, request *protocol.Request) (protocol.RequestResponse, error) {
	data, err := m.view.ReadRequest(context.Background(), request)
	if err != nil {
		return nil, err
	}
	m.requests.Add(1)
	m.transferred.Add(uint64(len(data)))
	return &wireResponse{data: data, done: make(chan struct{})}, nil
}
func (m *daemonModel) wait(t *testing.T, ctx context.Context, wanted func(protocol.FileInfo) bool) protocol.FileInfo {
	t.Helper()
	for {
		select {
		case info := <-m.indexes:
			if wanted(info) {
				return info
			}
		case err := <-m.closed:
			t.Fatal(err)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
	}
}

type referenceDaemon struct {
	folder, api, bep, key string
	device                protocol.DeviceID
	client                *http.Client
}

func referenceEnvironment() []string {
	var result []string
	for _, value := range os.Environ() {
		name := strings.ToUpper(strings.SplitN(value, "=", 2)[0])
		if !strings.HasPrefix(name, "ST") && !strings.HasPrefix(name, "SYRNCK_") {
			result = append(result, value)
		}
	}
	return append(result, "STMONITORED=yes", "STNORESTART=1", "STNOUPGRADE=1")
}
func freeAddress(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	return listener.Addr().String()
}
func startReference(t *testing.T, ctx context.Context, binary string, peer protocol.DeviceID, name string, data []byte) *referenceDaemon {
	t.Helper()
	return startReferencePeers(t, ctx, binary, []protocol.DeviceID{peer}, name, data, false)
}
func startReferencePeers(t *testing.T, ctx context.Context, binary string, peers []protocol.DeviceID, name string, data []byte, encrypted bool) *referenceDaemon {
	t.Helper()
	return startReferencePeersWithPasswords(t, ctx, binary, peers, name, data, encrypted, nil)
}
func startReferencePeersWithPasswords(t *testing.T, ctx context.Context, binary string, peers []protocol.DeviceID, name string, data []byte, encrypted bool, passwords map[protocol.DeviceID]string) *referenceDaemon {
	t.Helper()
	root := tempDirectory(t)
	home := filepath.Join(root, "home")
	folder := filepath.Join(root, "folder")
	for _, directory := range []string{home, filepath.Join(folder, ".stfolder"), filepath.Dir(filepath.Join(folder, filepath.FromSlash(name)))} {
		if err := os.MkdirAll(directory, 0700); err != nil {
			t.Fatal(err)
		}
	}
	if name != "" {
		if err := os.WriteFile(filepath.Join(folder, filepath.FromSlash(name)), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	generate := exec.CommandContext(ctx, binary, "generate", "--home", home, "--no-port-probing")
	generate.Env = referenceEnvironment()
	hideReferenceProcess(generate)
	if output, err := generate.CombinedOutput(); err != nil {
		t.Fatalf("reference generate: %v\n%s", err, output)
	}
	certificate, err := tls.LoadX509KeyPair(filepath.Join(home, "cert.pem"), filepath.Join(home, "key.pem"))
	if err != nil {
		t.Fatal(err)
	}
	ref := &referenceDaemon{folder: folder, bep: freeAddress(t), api: "http://" + freeAddress(t), device: protocol.NewDeviceID(certificate.Certificate[0]),
		client: &http.Client{Timeout: time.Second, Transport: &http.Transport{Proxy: nil}, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}}
	t.Cleanup(func() { ref.client.CloseIdleConnections() })
	var secret [32]byte
	if _, err := rand.Read(secret[:]); err != nil {
		t.Fatal(err)
	}
	ref.key = hex.EncodeToString(secret[:])
	configData, err := os.ReadFile(filepath.Join(home, "config.xml"))
	if err != nil {
		t.Fatal(err)
	}
	var rootVersion struct {
		Version int `xml:"version,attr"`
	}
	if err := xml.Unmarshal(configData, &rootVersion); err != nil {
		t.Fatal(err)
	}
	var escaped bytes.Buffer
	if err := xml.EscapeText(&escaped, []byte(folder)); err != nil {
		t.Fatal(err)
	}
	folderType := "sendreceive"
	if encrypted {
		folderType = "receiveencrypted"
	}
	shared := fmt.Sprintf(`<device id="%s"/>`, ref.device)
	devices := fmt.Sprintf(`<device id="%s" name="reference"><address>tcp://127.0.0.1:1</address></device>`, ref.device)
	for _, peer := range peers {
		var password bytes.Buffer
		if err := xml.EscapeText(&password, []byte(passwords[peer])); err != nil {
			t.Fatal(err)
		}
		shared += fmt.Sprintf(`<device id="%s"><encryptionPassword>%s</encryptionPassword></device>`, peer, password.String())
		devices += fmt.Sprintf(`<device id="%s" name="syrnck"><address>tcp://127.0.0.1:1</address></device>`, peer)
	}
	configuration := fmt.Sprintf(`<configuration version="%d">
<folder id="project" path="%s" type="%s" rescanIntervalS="3600" fsWatcherEnabled="false" ignorePerms="true">%s<markerName>.stfolder</markerName></folder>
%s<gui enabled="true" tls="false"><address>%s</address><apikey>%s</apikey></gui>
<options><listenAddress>tcp://%s</listenAddress><globalAnnounceEnabled>false</globalAnnounceEnabled><localAnnounceEnabled>false</localAnnounceEnabled><relaysEnabled>false</relaysEnabled><natEnabled>false</natEnabled><startBrowser>false</startBrowser><crashReportingEnabled>false</crashReportingEnabled><urAccepted>-1</urAccepted><autoUpgradeIntervalH>0</autoUpgradeIntervalH></options></configuration>`,
		rootVersion.Version, escaped.String(), folderType, shared, devices, strings.TrimPrefix(ref.api, "http://"), ref.key, ref.bep)
	if err := os.WriteFile(filepath.Join(home, "config.xml"), []byte(configuration), 0600); err != nil {
		t.Fatal(err)
	}
	logPath := filepath.Join(root, "daemon.log")
	log, err := os.Create(logPath)
	if err != nil {
		t.Fatal(err)
	}
	// Console visibility is configured by hideReferenceProcess on Windows.
	// Upstream's --no-console flag does not exist on Linux or macOS.
	command := exec.CommandContext(ctx, binary, "serve", "--home", home, "--no-browser", "--no-restart", "--no-upgrade", "--log-file=-")
	command.Env = referenceEnvironment()
	command.Stdout = log
	command.Stderr = log
	hideReferenceProcess(command)
	if err := command.Start(); err != nil {
		_ = log.Close()
		t.Fatal(err)
	}
	exited := make(chan struct{})
	var exitError error
	go func() { exitError = command.Wait(); close(exited) }()
	t.Cleanup(func() {
		_ = command.Process.Kill()
		<-exited
		_ = log.Close()
		if t.Failed() {
			text, _ := os.ReadFile(logPath)
			t.Logf("reference daemon: %s", text)
		}
	})
	for {
		request, _ := http.NewRequestWithContext(ctx, "GET", ref.api+"/rest/system/ping", nil)
		request.Header.Set("X-API-Key", ref.key)
		response, err := ref.client.Do(request)
		if err == nil {
			_ = response.Body.Close()
			if response.StatusCode == http.StatusOK {
				return ref
			}
		}
		select {
		case <-exited:
			t.Fatal("reference daemon exited during API startup: ", exitError)
		case <-ctx.Done():
			t.Fatal("reference API startup: ", ctx.Err())
		case <-time.After(30 * time.Millisecond):
		}
	}
}
func (r *referenceDaemon) scan(t *testing.T, ctx context.Context) {
	t.Helper()
	request, _ := http.NewRequestWithContext(ctx, "POST", r.api+"/rest/db/scan?folder="+url.QueryEscape("project"), nil)
	request.Header.Set("X-API-Key", r.key)
	response, err := r.client.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
		t.Fatalf("reference scan: %s %s", response.Status, data)
	}
}
func (r *referenceDaemon) waitFile(t *testing.T, ctx context.Context, name string, data []byte) {
	t.Helper()
	for {
		actual, err := os.ReadFile(filepath.Join(r.folder, filepath.FromSlash(name)))
		if err == nil && bytes.Equal(actual, data) {
			return
		}
		select {
		case <-ctx.Done():
			t.Fatal("reference puller did not reconstruct file: ", ctx.Err())
		case <-time.After(30 * time.Millisecond):
		}
	}
}
