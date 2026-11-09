package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"syscall"
	"time"
	"unicode/utf8"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
	bep "syrnck.local/syncthing"
)

type configuration struct {
	Version               int    `json:"version"`
	Directory             string `json:"directory"`
	Storage               string `json:"storage"`
	Workspace             string `json:"workspace"`
	Folder                string `json:"folder"`
	Certificate           string `json:"certificate"`
	PrivateKey            string `json:"private_key"`
	Peer                  string `json:"peer"`
	Mode                  string `json:"mode"`
	Address               string `json:"address"`
	KeyEnv                string `json:"key_env"`
	UsePermissions        bool   `json:"use_permissions"`
	PreserveCheckoutEdits bool   `json:"preserve_checkout_edits"`
	PollMilliseconds      uint32 `json:"poll_ms"`
	RetryMinMilliseconds  uint32 `json:"retry_min_ms"`
	RetryMaxMilliseconds  uint32 `json:"retry_max_ms"`
}

func limitedFile(path string, limit int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, errors.New("input file exceeds size limit")
	}
	return data, nil
}

// Reject ambiguous duplicate members before decoding the typed configuration.
func jsonValue(decoder *json.Decoder, depth int) error {
	if depth > 32 {
		return errors.New("configuration nesting exceeds limit")
	}
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, ok := token.(json.Delim)
	if !ok {
		return nil
	}
	switch delimiter {
	case '{':
		seen := map[string]bool{}
		for decoder.More() {
			key, err := decoder.Token()
			if err != nil {
				return err
			}
			name, ok := key.(string)
			if !ok || seen[name] {
				return errors.New("configuration contains duplicate or invalid members")
			}
			seen[name] = true
			if err := jsonValue(decoder, depth+1); err != nil {
				return err
			}
		}
	case '[':
		for decoder.More() {
			if err := jsonValue(decoder, depth+1); err != nil {
				return err
			}
		}
	default:
		return errors.New("invalid configuration structure")
	}
	_, err = decoder.Token()
	return err
}

func loadConfiguration(path string) (configuration, error) {
	var config configuration
	data, err := limitedFile(path, 64*1024)
	if err != nil {
		return config, err
	}
	if !utf8.Valid(data) {
		return config, errors.New("configuration must be UTF-8")
	}
	check := json.NewDecoder(bytes.NewReader(data))
	check.UseNumber()
	if err := jsonValue(check, 0); err != nil {
		return config, err
	}
	if _, err := check.Token(); !errors.Is(err, io.EOF) {
		return config, errors.New("configuration must contain one JSON object")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&config); err != nil {
		return config, err
	}
	if config.Version != 1 || config.Directory == "" || config.Folder == "" || config.Certificate == "" || config.PrivateKey == "" {
		return config, errors.New("configuration version 1, directory, folder, certificate and private_key are required")
	}
	if config.Mode == "" {
		config.Mode = "dial"
	}
	if config.Mode != "dial" && config.Mode != "listen" {
		return config, errors.New("mode must be dial or listen")
	}
	if config.PollMilliseconds != 0 && (config.PollMilliseconds < 50 || config.PollMilliseconds > 60000) {
		return config, errors.New("poll_ms must be 50..60000 or zero for default")
	}
	minimum, maximum := config.RetryMinMilliseconds, config.RetryMaxMilliseconds
	if minimum == 0 {
		minimum = 500
	}
	if maximum == 0 {
		maximum = 30000
	}
	if minimum < 50 || maximum > 60000 || minimum > maximum {
		return config, errors.New("retry bounds must be in 50..60000 milliseconds")
	}
	host, port, err := net.SplitHostPort(config.Address)
	if err != nil {
		return config, errors.New("address must be host:port")
	}
	number, err := strconv.ParseUint(port, 10, 16)
	if err != nil || (config.Mode == "dial" && (host == "" || number == 0)) {
		return config, errors.New("invalid TCP address")
	}
	if config.KeyEnv == "" {
		config.KeyEnv = "SYRNCK_KEY"
	}
	if len(config.KeyEnv) > 128 {
		return config, errors.New("invalid key environment variable name")
	}
	for i, c := range config.KeyEnv {
		if !(c == '_' || c >= 'A' && c <= 'Z' || i > 0 && c >= '0' && c <= '9') {
			return config, errors.New("invalid key environment variable name")
		}
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return config, err
	}
	resolve := func(path string) string {
		if filepath.IsAbs(path) {
			return filepath.Clean(path)
		}
		return filepath.Join(filepath.Dir(absolute), path)
	}
	config.Directory = resolve(config.Directory)
	if config.Storage == "" {
		config.Storage = filepath.Join(config.Directory, ".syrnck")
	} else {
		config.Storage = resolve(config.Storage)
	}
	config.Certificate = resolve(config.Certificate)
	config.PrivateKey = resolve(config.PrivateKey)
	return config, nil
}

func certificateID(path string) (string, error) {
	data, err := limitedFile(path, 1024*1024)
	if err != nil {
		return "", err
	}
	block, rest := pem.Decode(data)
	if block == nil || block.Type != "CERTIFICATE" || len(bytes.TrimSpace(rest)) != 0 {
		return "", errors.New("one PEM device certificate is required")
	}
	cert, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		return "", errors.New("invalid device certificate")
	}
	return protocol.NewDeviceID(cert.Raw).String(), nil
}

func runService(ctx context.Context, config configuration, output io.Writer) (result error) {
	// Device signing material must never enter ordinary source snapshots.
	directory, err := canonicalExisting(config.Directory)
	if err != nil {
		return fmt.Errorf("resolve workspace directory: %w", err)
	}
	storage, err := canonicalExisting(config.Storage)
	if err != nil {
		return fmt.Errorf("resolve Store directory: %w", err)
	}
	privatePath, err := canonicalExisting(config.PrivateKey)
	if err != nil {
		return fmt.Errorf("resolve private device key: %w", err)
	}
	selected, err := keyInWorkingTree(directory, storage, privatePath)
	if err != nil {
		return err
	}
	if selected {
		return errors.New("private device key must be outside the workspace or inside its protected Store")
	}
	certificate, err := limitedFile(config.Certificate, 1024*1024)
	if err != nil {
		return err
	}
	private, err := limitedFile(config.PrivateKey, 1024*1024)
	if err != nil {
		return err
	}
	identity, err := tls.X509KeyPair(certificate, private)
	for i := range private {
		private[i] = 0
	}
	if err != nil {
		return errors.New("cannot load matching device certificate and key")
	}
	if len(identity.Certificate) != 1 {
		return errors.New("one leaf device certificate is required")
	}
	local := protocol.NewDeviceID(identity.Certificate[0])
	peer, err := protocol.DeviceIDFromString(config.Peer)
	if err != nil || peer == local {
		return errors.New("an explicit different paired peer ID is required")
	}
	if runtime.GOOS == "windows" && os.Getenv("SYRNCK_LIBRARY") == "" {
		executable, err := os.Executable()
		if err != nil {
			return err
		}
		if err := os.Setenv("SYRNCK_LIBRARY", filepath.Join(filepath.Dir(executable), "syrnck_ffi.dll")); err != nil {
			return err
		}
	}
	provider := func(ctx context.Context) (*syrnck.Store, error) {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		var key []byte
		defer func() {
			for i := range key {
				key[i] = 0
			}
		}()
		if text := os.Getenv(config.KeyEnv); text != "" {
			var err error
			key, err = hex.DecodeString(text)
			if err != nil || len(key) != 32 {
				return nil, errors.New("storage key environment variable must contain 64 hexadecimal characters")
			}
		}
		return syrnck.Open(config.Storage, syrnck.Options{Key: key})
	}
	folder, err := bep.NewFolder(ctx, bep.FolderOptions{Directory: config.Directory, Storage: config.Storage, Workspace: config.Workspace,
		Catalog:   bep.CatalogOptions{Folder: config.Folder, LocalDevice: local, Peers: []protocol.DeviceID{peer}},
		OpenStore: provider, UsePermissions: config.UsePermissions, PreserveCheckoutEdits: config.PreserveCheckoutEdits})
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, folder.Close()) }()
	var listener net.Listener
	connect := func(ctx context.Context) (net.Conn, error) {
		return (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", config.Address)
	}
	if config.Mode == "listen" {
		listener, err = net.Listen("tcp", config.Address)
		if err != nil {
			return err
		}
		defer listener.Close()
		connect = func(ctx context.Context) (net.Conn, error) {
			stop := context.AfterFunc(ctx, func() { _ = listener.Close() })
			defer stop()
			return listener.Accept()
		}
	}
	service, err := bep.OpenFolderService(ctx, folder, bep.FolderServiceOptions{Connect: connect,
		Session:  bep.FolderSessionOptions{Device: bep.DeviceOptions{Certificate: identity, RemoteDevice: peer, Server: config.Mode == "listen"}, PollInterval: time.Duration(config.PollMilliseconds) * time.Millisecond},
		RetryMin: time.Duration(config.RetryMinMilliseconds) * time.Millisecond, RetryMax: time.Duration(config.RetryMaxMilliseconds) * time.Millisecond})
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, service.Close()) }()
	encode := json.NewEncoder(output)
	address := config.Address
	if listener != nil {
		address = listener.Addr().String()
	}
	if err := encode.Encode(map[string]any{"event": "ready", "device_id": local.String(), "peer": peer.String(), "folder": config.Folder, "mode": config.Mode, "address": address}); err != nil {
		return err
	}
	reports, failures, events := service.Reports(), service.Errors(), service.Events()
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-service.Done():
			return nil
		case status, ok := <-reports:
			if !ok {
				reports = nil
				continue
			}
			if err := encode.Encode(map[string]any{"event": "folder", "status": status}); err != nil {
				return err
			}
		case failure, ok := <-failures:
			if !ok {
				failures = nil
				continue
			}
			if err := encode.Encode(map[string]any{"event": "error", "message": failure.Error()}); err != nil {
				return err
			}
		case event, ok := <-events:
			if !ok {
				events = nil
				continue
			}
			if err := encode.Encode(map[string]any{"event": "connection", "status": event}); err != nil {
				return err
			}
		}
	}
}

// Compare physical ancestors rather than path spelling. Case-insensitive and
// normalization-insensitive filesystems can alias distinct input strings.
func keyInWorkingTree(directory, storage, privatePath string) (bool, error) {
	working, err := os.Stat(directory)
	if err != nil {
		return false, err
	}
	protected, err := os.Stat(storage)
	if err != nil {
		return false, err
	}
	for parent := filepath.Dir(privatePath); ; {
		info, err := os.Stat(parent)
		if err != nil {
			return false, err
		}
		if os.SameFile(info, protected) {
			return false, nil
		}
		if os.SameFile(info, working) {
			return true, nil
		}
		next := filepath.Dir(parent)
		if next == parent {
			return false, nil
		}
		parent = next
	}
}

func execute(ctx context.Context, args []string, output, diagnostics io.Writer) int {
	global := flag.NewFlagSet("syrnck-bep", flag.ContinueOnError)
	global.SetOutput(io.Discard)
	defaultLanguage := os.Getenv("SYRNCK_LANG")
	if defaultLanguage == "" {
		defaultLanguage = "en"
	}
	language := global.String("lang", defaultLanguage, "")
	usage := func() {
		if *language == "fr" {
			fmt.Fprintln(output, "Service de synchronisation BEP Syrnck\nUsage : syrnck-bep [--lang en|fr] run --config FICHIER\n        syrnck-bep [--lang en|fr] device-id --certificate FICHIER\nLe Store doit être initialisé ; configuration locale JSON version 1. Arrêt par Ctrl+C.")
		} else {
			fmt.Fprintln(output, "Syrnck BEP synchronization service\nUsage: syrnck-bep [--lang en|fr] run --config FILE\n       syrnck-bep [--lang en|fr] device-id --certificate FILE\nRequires an initialized Store and local JSON configuration version 1. Stop with Ctrl+C.")
		}
		if *language == "fr" {
			fmt.Fprintln(output, "Manuel local : syrnck --lang fr help syncthing --section standalone-service")
		} else {
			fmt.Fprintln(output, "Local manual: syrnck help syncthing --section standalone-service")
		}
	}
	fail := func(err error) int {
		prefix := "Error:"
		if *language == "fr" {
			prefix = "Erreur :"
		}
		fmt.Fprintln(diagnostics, prefix, humanError(*language, err))
		return 1
	}
	if err := global.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			usage()
			return 0
		}
		return fail(err)
	}
	if *language != "en" && *language != "fr" {
		return fail(errors.New("--lang must be en or fr"))
	}
	args = global.Args()
	if len(args) == 0 {
		usage()
		return 0
	}
	flags := flag.NewFlagSet(args[0], flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	var path *string
	switch args[0] {
	case "run":
		path = flags.String("config", "", "")
	case "device-id":
		path = flags.String("certificate", "", "")
	default:
		return fail(errors.New("expected run or device-id"))
	}
	if err := flags.Parse(args[1:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			usage()
			return 0
		}
		return fail(err)
	}
	if *path == "" || len(flags.Args()) != 0 {
		return fail(errors.New("a configuration or certificate file is required"))
	}
	if args[0] == "device-id" {
		id, err := certificateID(*path)
		if err != nil {
			return fail(err)
		}
		fmt.Fprintln(output, id)
		return 0
	}
	config, err := loadConfiguration(*path)
	if err != nil {
		return fail(err)
	}
	if err := runService(ctx, config, output); err != nil {
		return fail(err)
	}
	return 0
}

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	code := execute(ctx, os.Args[1:], os.Stdout, os.Stderr)
	stop()
	os.Exit(code)
}
