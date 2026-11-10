package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"io"
	"math/big"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

func testIdentity(t *testing.T, root string) (string, string, string) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{SerialNumber: big.NewInt(1), NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature}
	der, err := x509.CreateCertificate(rand.Reader, template, template, public, private)
	if err != nil {
		t.Fatal(err)
	}
	key, err := x509.MarshalPKCS8PrivateKey(private)
	if err != nil {
		t.Fatal(err)
	}
	certPath, keyPath := filepath.Join(root, "cert.pem"), filepath.Join(root, "key.pem")
	if err := os.WriteFile(certPath, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: key}), 0600); err != nil {
		t.Fatal(err)
	}
	return certPath, keyPath, protocol.NewDeviceID(der).String()
}

func TestStandaloneProcessRetainsOfflineVersionsAfterHardTermination(t *testing.T) {
	executable := os.Getenv("SYRNCK_TEST_BEP_SERVICE")
	if executable == "" {
		t.Skip("build syrnck-bep and set SYRNCK_TEST_BEP_SERVICE")
	}
	if !filepath.IsAbs(executable) {
		t.Fatal("standalone executable path must be absolute")
	}
	for _, protectedSigner := range []bool{false, true} {
		for _, encrypted := range []bool{false, true} {
			t.Run(map[bool]string{false: "plain", true: "encrypted"}[encrypted]+"-"+map[bool]string{false: "raw", true: "protected"}[protectedSigner], func(t *testing.T) {
				root := t.TempDir()
				directory := filepath.Join(root, "work")
				if err := os.Mkdir(directory, 0700); err != nil {
					t.Fatal(err)
				}
				cert, key, _ := testIdentity(t, root)
				_, _, peer := testIdentity(t, t.TempDir())
				var storageKey []byte
				if encrypted {
					storageKey = bytes.Repeat([]byte{91}, 32)
				}
				t.Setenv("SYRNCK_BEP_PROCESS_TEST_KEY", hex.EncodeToString(storageKey))
				storage := filepath.Join(directory, ".syrnck")
				store, err := syrnck.Open(storage, syrnck.Options{Create: true, Key: storageKey})
				if err != nil {
					t.Fatal(err)
				}
				store.Close()
				config := configuration{Version: 1, Directory: directory, Storage: storage, Folder: "project", Certificate: cert, PrivateKey: key, Peer: peer, Mode: "listen", Address: "127.0.0.1:0", KeyEnv: "SYRNCK_BEP_PROCESS_TEST_KEY", Author: &syrnck.NativeIdentity{Name: "Process author", Email: "process@example.invalid", TimezoneOffset: -60}, SigningKeyEnv: "SYRNCK_BEP_PROCESS_SIGNING_KEY"}
				t.Setenv("SYRNCK_BEP_PROCESS_SIGNING_KEY", hex.EncodeToString(bytes.Repeat([]byte{34}, 32)))
				if protectedSigner {
					config.SigningKeyEnv = ""
					config.SigningKeyFile = filepath.Join(root, "author.syrsig")
					config.SigningKeyPasswordEnv = "SYRNCK_BEP_PROCESS_SIGNING_PASSWORD"
					password := []byte("standalone signing credential")
					envelope, err := syrnck.ProtectSigningKey(bytes.Repeat([]byte{34}, 32), password, syrnck.PasswordKdfOptions{})
					if err != nil {
						t.Fatal(err)
					}
					if err := os.WriteFile(config.SigningKeyFile, envelope, 0600); err != nil {
						t.Fatal(err)
					}
					t.Setenv(config.SigningKeyPasswordEnv, string(password))
				}
				document, err := json.Marshal(config)
				if err != nil {
					t.Fatal(err)
				}
				configPath := filepath.Join(root, "service.json")
				if err := os.WriteFile(configPath, document, 0600); err != nil {
					t.Fatal(err)
				}
				previous := ""
				var manifests []string
				contents := []string{"first offline process version", "second offline process version"}
				for _, content := range contents {
					if err := os.WriteFile(filepath.Join(directory, "asset.bin"), []byte(content), 0600); err != nil {
						t.Fatal(err)
					}
					ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
					command := exec.CommandContext(ctx, executable, "run", "--config", configPath)
					hideProcess(command)
					command.Env = os.Environ()
					if runtime.GOOS == "windows" {
						// Qualify automatic adjacent native library lookup in the child.
						filtered := command.Env[:0]
						for _, entry := range command.Env {
							if !strings.HasPrefix(strings.ToUpper(entry), "SYRNCK_LIBRARY=") {
								filtered = append(filtered, entry)
							}
						}
						command.Env = filtered
					}
					var diagnostics bytes.Buffer
					command.Stderr = &diagnostics
					output, err := command.StdoutPipe()
					if err != nil {
						cancel()
						t.Fatal(err)
					}
					if err := command.Start(); err != nil {
						cancel()
						t.Fatal(err)
					}
					var ready struct{ Event string }
					decodeError := json.NewDecoder(output).Decode(&ready)
					killError := command.Process.Kill()
					waitError := command.Wait()
					cancel()
					_ = output.Close()
					if decodeError != nil || ready.Event != "ready" || killError != nil || waitError == nil {
						t.Fatal("standalone process did not publish then terminate", decodeError, ready, killError, waitError, diagnostics.String())
					}
					store, err = syrnck.Open(storage, syrnck.Options{Key: storageKey})
					if err != nil {
						t.Fatal(err)
					}
					refs, err := store.Refs()
					if err != nil {
						store.Close()
						t.Fatal(err)
					}
					root, ok := refs["heads/main"]
					if !ok {
						store.Close()
						t.Fatal("terminated process lost native head")
					}
					data, err := store.Get(root)
					if err != nil {
						store.Close()
						t.Fatal(err)
					}
					var revision struct {
						Version   int `json:"version"`
						Signature struct {
							PublicKey string `json:"public_key"`
						} `json:"signature"`
						Parents []string `json:"parents"`
						Files   map[string]struct {
							Manifest string `json:"manifest"`
						} `json:"files"`
					}
					if err := json.Unmarshal(data, &revision); err != nil {
						store.Close()
						t.Fatal(err)
					}
					expected := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{34}, 32)).Public().(ed25519.PublicKey)
					if revision.Version != 2 || revision.Signature.PublicKey != hex.EncodeToString(expected) {
						store.Close()
						t.Fatal("standalone capture did not use native signing identity", string(data))
					}
					if previous != "" && (len(revision.Parents) != 1 || revision.Parents[0] != previous) {
						store.Close()
						t.Fatal("restarted process replaced historical parent", revision.Parents)
					}
					previous = root.ID
					manifests = append(manifests, revision.Files["asset.bin"].Manifest)
					for i, manifest := range manifests {
						got, err := store.ReadRange(manifest, 0, uint64(len(contents[i])))
						if err != nil || string(got) != contents[i] {
							store.Close()
							t.Fatal("old process version is not reconstructible", i, err)
						}
					}
					if err := store.Close(); err != nil {
						t.Fatal(err)
					}
				}
			})
		}
	}
}

func TestConfigurationIsStrictAndResolvesRelativePaths(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "service.json")
	valid := `{"version":1,"directory":"work","folder":"project","certificate":"cert.pem","private_key":"key.pem","peer":"explicit-pair","mode":"listen","address":"127.0.0.1:0"}`
	write := func(text string) {
		t.Helper()
		if err := os.WriteFile(path, []byte(text), 0600); err != nil {
			t.Fatal(err)
		}
	}
	write(valid)
	config, err := loadConfiguration(path)
	if err != nil || config.Directory != filepath.Join(root, "work") || config.Storage != filepath.Join(root, "work", ".syrnck") || config.KeyEnv != "SYRNCK_KEY" {
		t.Fatal(config, err)
	}
	for _, text := range []string{
		strings.Replace(valid, `"version":1`, `"version":1,"version":1`, 1),
		strings.Replace(valid, `"version":1`, `"version":1,"ver\u0073ion":1`, 1),
		strings.Replace(valid, `"version":1`, `"version":1,"unknown":true`, 1),
		strings.Replace(valid, `"version":1`, `"version":2`, 1),
		strings.Replace(valid, `"version":1`, `"version":1,"poll_ms":1`, 1),
		strings.Replace(valid, `"version":1`, `"version":1,"retry_min_ms":1000,"retry_max_ms":50`, 1),
		valid + "{}", "null", "[]", string([]byte{0xff}), strings.Repeat(" ", 65537),
	} {
		write(text)
		if _, err := loadConfiguration(path); err == nil {
			t.Fatalf("accepted invalid configuration: %.100s", text)
		}
	}
}

func TestHelpLanguagesAndDeviceIdentityWithoutOpeningStore(t *testing.T) {
	cert, _, id := testIdentity(t, t.TempDir())
	for _, language := range []string{"en", "fr"} {
		var output, diagnostics bytes.Buffer
		if code := execute(context.Background(), []string{"--lang", language, "--help"}, &output, &diagnostics); code != 0 {
			t.Fatal(code, diagnostics.String())
		}
		if language == "fr" && !strings.Contains(output.String(), "configuration locale") {
			t.Fatal("missing French help")
		}
		if language == "fr" {
			if code := execute(context.Background(), []string{"--lang", "fr", "run"}, io.Discard, &diagnostics); code == 0 || !strings.Contains(diagnostics.String(), "un fichier de configuration") {
				t.Fatal("French CLI startup diagnostic missing", code, diagnostics.String())
			}
			diagnostics.Reset()
		}
		output.Reset()
		if code := execute(context.Background(), []string{"--lang", language, "device-id", "--certificate", cert}, &output, &diagnostics); code != 0 || strings.TrimSpace(output.String()) != id {
			t.Fatal(code, output.String(), diagnostics.String())
		}
	}
	t.Setenv("SYRNCK_LANG", "fr")
	var output, diagnostics bytes.Buffer
	if code := execute(context.Background(), []string{"--help"}, &output, &diagnostics); code != 0 || !strings.Contains(output.String(), "configuration locale") {
		t.Fatal("environment language was not applied", code, output.String(), diagnostics.String())
	}
	output.Reset()
	if code := execute(context.Background(), []string{"--lang", "en", "--help"}, &output, &diagnostics); code != 0 || strings.Contains(output.String(), "configuration locale") {
		t.Fatal("explicit language did not override environment", code, output.String(), diagnostics.String())
	}
}

func TestCLIListenCancellationRetainsPlainAndEncryptedOfflineSnapshot(t *testing.T) {
	for _, provider := range []string{"plain", "encrypted", "protected"} {
		t.Run(provider, func(t *testing.T) {
			encrypted := provider != "plain"
			root := testDirectory(t)
			directory := filepath.Join(root, "work")
			if err := os.Mkdir(directory, 0700); err != nil {
				t.Fatal(err)
			}
			cert, key, _ := testIdentity(t, root)
			peerRoot := t.TempDir()
			_, _, peer := testIdentity(t, peerRoot)
			var storageKey []byte
			if encrypted {
				storageKey = bytes.Repeat([]byte{87}, 32)
			}
			t.Setenv("SYRNCK_BEP_TEST_STORAGE_KEY", hex.EncodeToString(storageKey))
			storage := filepath.Join(directory, ".syrnck")
			store, err := syrnck.Open(storage, syrnck.Options{Create: true, Key: storageKey})
			if err != nil {
				t.Fatal(err)
			}
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}
			if encrypted {
				material, err := os.ReadFile(key)
				if err != nil {
					t.Fatal(err)
				}
				key = filepath.Join(storage, "device-key.pem")
				if err := os.WriteFile(key, material, 0600); err != nil {
					t.Fatal(err)
				}
			}
			if err := os.WriteFile(filepath.Join(directory, "asset.bin"), []byte("offline workspace bytes"), 0600); err != nil {
				t.Fatal(err)
			}
			config := configuration{Version: 1, Directory: directory, Storage: storage, Folder: "project", Certificate: cert, PrivateKey: key, Peer: peer, Mode: "listen", Address: "127.0.0.1:0", KeyEnv: "SYRNCK_BEP_TEST_STORAGE_KEY"}
			if provider == "protected" {
				config.KeyFile = filepath.Join(storage, "clé-🔑.syrkey")
				config.KeyPasswordEnv = "SYRNCK_BEP_TEST_PASSWORD"
				envelope, err := syrnck.ProtectKey(storageKey, []byte("passphrase-🔑"), syrnck.PasswordKdfOptions{})
				if err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(config.KeyFile, envelope, 0600); err != nil {
					t.Fatal(err)
				}
				t.Setenv(config.KeyEnv, "")
				t.Setenv(config.KeyPasswordEnv, "passphrase-🔑")
			}
			encoded, err := json.Marshal(config)
			if err != nil {
				t.Fatal(err)
			}
			configPath := filepath.Join(root, "service.json")
			if err := os.WriteFile(configPath, encoded, 0600); err != nil {
				t.Fatal(err)
			}
			reader, writer, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			defer reader.Close()
			defer writer.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			var diagnostics bytes.Buffer
			done := make(chan int, 1)
			go func() { done <- execute(ctx, []string{"run", "--config", configPath}, writer, &diagnostics) }()
			var ready struct{ Event, Address string }
			decoded := make(chan error, 1)
			go func() { decoded <- json.NewDecoder(reader).Decode(&ready) }()
			select {
			case err := <-decoded:
				if err != nil {
					cancel()
					<-done
					t.Fatal(err, diagnostics.String())
				}
			case code := <-done:
				_ = writer.Close()
				<-decoded
				t.Fatal("CLI stopped before ready", code, diagnostics.String())
			case <-ctx.Done():
				cancel()
				<-done
				_ = writer.Close()
				<-decoded
				t.Fatal("CLI readiness timed out", diagnostics.String())
			}
			if ready.Event != "ready" || strings.HasSuffix(ready.Address, ":0") {
				t.Fatal("listener did not publish its bound address", ready)
			}
			cancel()
			select {
			case code := <-done:
				if code != 0 {
					t.Fatal(code, diagnostics.String())
				}
			case <-time.After(2 * time.Second):
				t.Fatal("CLI leaked listener or session workers")
			}
			store, err = syrnck.Open(storage, syrnck.Options{Key: storageKey})
			if err != nil {
				t.Fatal(err)
			}
			defer store.Close()
			refs, err := store.Refs()
			if err != nil {
				t.Fatal(err)
			}
			revision, ok := refs["heads/main"]
			if !ok {
				t.Fatal("offline native snapshot missing")
			}
			data, err := store.Get(revision)
			if err != nil {
				t.Fatal(err)
			}
			var native struct {
				Files map[string]any `json:"files"`
			}
			if err := json.Unmarshal(data, &native); err != nil || native.Files["asset.bin"] == nil || len(native.Files) != 1 {
				t.Fatal("offline snapshot omitted asset", err)
			}
		})
	}
}

func TestPrivateSigningKeyInWorkingTreeRefusedBeforeCapture(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, "work")
	if err := os.Mkdir(directory, 0700); err != nil {
		t.Fatal(err)
	}
	cert, key, _ := testIdentity(t, directory)
	_, _, peer := testIdentity(t, t.TempDir())
	storage := filepath.Join(directory, ".syrnck")
	store, err := syrnck.Open(storage, syrnck.Options{Create: true})
	if err != nil {
		t.Fatal(err)
	}
	store.Close()
	config := configuration{Directory: directory, Storage: storage, Certificate: cert, PrivateKey: key, Peer: peer, Folder: "project", Mode: "listen", Address: "127.0.0.1:0"}
	if err := runService(context.Background(), config, io.Discard); err == nil || !strings.Contains(err.Error(), "private device key must be outside") {
		t.Fatal("private signing key guard did not refuse before source capture", err)
	}
	t.Run("symlink-key", func(t *testing.T) {
		alias := filepath.Join(root, "outside-key.pem")
		if err := os.Symlink(key, alias); err != nil {
			t.Skip("local symlink creation unavailable", err)
		}
		linked := config
		linked.PrivateKey = alias
		if err := runService(context.Background(), linked, io.Discard); err == nil || !strings.Contains(err.Error(), "private device key must be outside") {
			t.Fatal("symbolic key alias bypassed capture guard", err)
		}
	})
	t.Run("case-alias-key", func(t *testing.T) {
		alias := filepath.Join(root, "WORK", filepath.Base(key))
		info, err := os.Stat(alias)
		original, originalErr := os.Stat(key)
		if err != nil || originalErr != nil || !os.SameFile(info, original) {
			t.Skip("filesystem has no equivalent case alias")
		}
		aliased := config
		aliased.PrivateKey = alias
		if err := runService(context.Background(), aliased, io.Discard); err == nil || !strings.Contains(err.Error(), "private device key must be outside") {
			t.Fatal("case alias bypassed private-key source guard", err)
		}
	})
	store, err = syrnck.Open(storage, syrnck.Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	refs, err := store.Refs()
	if err != nil || len(refs) != 0 {
		t.Fatal("unsafe configuration changed retained roots", refs, err)
	}
}
