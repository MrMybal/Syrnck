package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	syrnck "syrnck.local/bindings/go"
)

func TestEncryptedReceiverConfigurationRejectsPlaintextRoles(t *testing.T) {
	path := filepath.Join(t.TempDir(), "receiver.json")
	base := configuration{Version: 1, Storage: "store", Folder: "project", Certificate: "cert.pem", PrivateKey: "device.pem", ReceiveEncrypted: true, Mode: "listen", Address: "127.0.0.1:0"}
	write := func(config configuration) {
		t.Helper()
		data, err := json.Marshal(config)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	write(base)
	loaded, err := loadConfiguration(path)
	if err != nil || loaded.Directory != "" || loaded.FolderPasswordEnv != "" || loaded.Storage != filepath.Join(filepath.Dir(path), "store") {
		t.Fatal("opaque config invented a workspace or password", loaded, err)
	}
	for _, change := range []func(*configuration){
		func(c *configuration) { c.Storage = "" },
		func(c *configuration) { c.Directory = "work" },
		func(c *configuration) { c.Workspace = "private" },
		func(c *configuration) { c.UsePermissions = true },
		func(c *configuration) { c.PreserveCheckoutEdits = true },
		func(c *configuration) { c.ConflictCopies = true },
		func(c *configuration) { c.ResumeDownloads = true },
		func(c *configuration) { c.UntrustedPeer = true },
		func(c *configuration) { c.FolderPasswordEnv = "PASSWORD" },
		func(c *configuration) { c.Author = &syrnck.NativeIdentity{} },
		func(c *configuration) { c.SigningKeyEnv = "SIGNER" },
		func(c *configuration) { c.SigningKeyFile = "author.syrsig" },
		func(c *configuration) { c.SigningKeyPasswordEnv = "SIGNER_PASSWORD" },
	} {
		bad := base
		change(&bad)
		write(bad)
		if _, err := loadConfiguration(path); err == nil {
			t.Fatal("mixed opaque/plaintext role accepted", bad)
		}
		var output bytes.Buffer
		err := runService(context.Background(), bad, &output)
		if err == nil || output.Len() != 0 || !strings.Contains(humanError("fr", err), "receive_encrypted") {
			t.Fatal("mixed role touched nonexistent paths or started service", err)
		}
		if humanError("fr", err) == err.Error() {
			t.Fatal("receiver diagnostic untranslated", err)
		}
	}
}
