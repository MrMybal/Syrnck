package syrnck

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"testing"
)

func TestPreparedNativeRevisionsUseHandleIdentityAndNativeSignatures(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			var key []byte
			if encrypted {
				key = bytes.Repeat([]byte{43}, 32)
			}
			root := filepath.Join(tempDirectory(t), "store")
			store := openTest(t, root, key)
			manifest, err := store.PrepareBytes([]byte("native asset"), Chunking{})
			if err != nil {
				t.Fatal(err)
			}
			draft := json.RawMessage(fmt.Sprintf(`{"version":1,"parents":[],"message":"integration","timestamp":42,"files":{"asset":{"manifest":%q,"executable":false}}}`, manifest))
			legacy, err := store.PrepareNativeRevision(draft)
			if err != nil {
				t.Fatal(err)
			}
			raw, err := store.Put("revision", draft)
			if err != nil || raw != legacy {
				t.Fatal("unconfigured preparation changed legacy identity", err)
			}
			identity := NativeIdentity{Name: "Équipe locale", Email: "author@example.invalid", TimezoneOffset: -120}
			seed := bytes.Repeat([]byte{27}, 32)
			public := ed25519.NewKeyFromSeed(seed).Public().(ed25519.PublicKey)
			if err := store.UseNativeIdentity(identity, seed); err != nil {
				t.Fatal(err)
			}
			clear(seed) // The native signer must no longer refer to Go's input.
			metadata, err := store.NativeIdentityMetadata()
			if err != nil || !bytes.Contains(metadata, []byte(hex.EncodeToString(public))) {
				t.Fatal("public identity differs", err, string(metadata))
			}
			if bytes.Contains(metadata, []byte(hex.EncodeToString(bytes.Repeat([]byte{27}, 32)))) {
				t.Fatal("identity metadata exported a seed")
			}
			beforeRefs, err := store.Refs()
			if err != nil {
				t.Fatal(err)
			}
			signed, err := store.PrepareNativeRevision(draft)
			if err != nil {
				t.Fatal(err)
			}
			data, err := store.Get(signed)
			if err != nil {
				t.Fatal(err)
			}
			var revision struct {
				Version   int             `json:"version"`
				Author    NativeIdentity  `json:"author"`
				Committer NativeIdentity  `json:"committer"`
				Signature json.RawMessage `json:"signature"`
			}
			if err := json.Unmarshal(data, &revision); err != nil {
				t.Fatal(err)
			}
			if revision.Version != 2 || revision.Author != identity || revision.Committer != identity {
				t.Fatal("native authored stamps differ", string(data))
			}
			var signature struct {
				PublicKey string `json:"public_key"`
				Value     string `json:"value"`
			}
			if err := json.Unmarshal(revision.Signature, &signature); err != nil {
				t.Fatal(err)
			}
			if signature.PublicKey != hex.EncodeToString(public) {
				t.Fatal("wrong native Ed25519 public key")
			}
			unsigned := bytes.Replace(data, revision.Signature, []byte("null"), 1)
			unsignedFile, err := store.PrepareBytes(unsigned, Chunking{})
			if err != nil {
				t.Fatal(err)
			}
			unsignedMetadata, err := store.Get(ObjectKey{Kind: "manifest", ID: unsignedFile})
			if err != nil {
				t.Fatal(err)
			}
			var hash struct {
				FileHash string `json:"file_hash"`
			}
			if err := json.Unmarshal(unsignedMetadata, &hash); err != nil {
				t.Fatal(err)
			}
			proof, err := hex.DecodeString(signature.Value)
			if err != nil || !ed25519.Verify(public, []byte("Syrnck native revision signature v1:"+hash.FileHash), proof) {
				t.Fatal("Go could not independently verify native signature", err)
			}
			for _, invalid := range []NativeIdentity{{Name: "", Email: identity.Email}, {Name: "bad\nidentity", Email: identity.Email}, {Name: identity.Name, Email: identity.Email, TimezoneOffset: 1440}} {
				if err := store.UseNativeIdentity(invalid, nil); err == nil {
					t.Fatal("invalid identity accepted")
				}
			}
			if err := store.UseNativeIdentity(identity, []byte{1}); err == nil {
				t.Fatal("short signing seed accepted")
			}
			if after, err := store.NativeIdentityMetadata(); err != nil || !bytes.Equal(after, metadata) {
				t.Fatal("invalid replacement changed identity", err)
			}
			if _, err := store.PrepareNativeRevision(data); err == nil {
				t.Fatal("existing v2 revision silently reauthored")
			}
			ctx, cancel := context.WithCancel(context.Background())
			cancel()
			if _, err := store.PrepareNativeRevisionContext(ctx, draft); !errors.Is(err, context.Canceled) {
				t.Fatal(err)
			}
			for _, invalid := range []json.RawMessage{[]byte(`{"version":1,"version":1}`), bytes.Replace(draft, []byte(manifest), []byte(string(bytes.Repeat([]byte{'a'}, 64))), 1)} {
				if result, err := store.PrepareNativeRevision(invalid); err == nil || result.ID != "" {
					t.Fatal("invalid dependency/draft was prepared", result, err)
				}
			}
			afterRefs, err := store.Refs()
			beforeJSON, _ := json.Marshal(beforeRefs)
			afterJSON, _ := json.Marshal(afterRefs)
			if err != nil || !bytes.Equal(beforeJSON, afterJSON) {
				t.Fatal("preparation or identity mutated refs", err)
			}
			if applied, err := store.CompareRefsContext(context.Background(), []RefUpdate{{Name: "heads/signed", Value: &signed}}); err != nil || !applied {
				t.Fatal("signed revision retention failed", err)
			}
			if err := store.ConfigureNativeIdentity(identity, NativeIdentityOptions{RequireSignature: true}); err != nil {
				t.Fatal(err)
			}
			if result, err := store.PrepareNativeRevision(draft); err == nil || result.ID != "" {
				t.Fatal("missing required signing key produced a revision", result, err)
			}
			if _, err := store.Get(signed); err != nil {
				t.Fatal("public verification required private signing key", err)
			}
			if err := store.ClearNativeIdentity(); err != nil {
				t.Fatal(err)
			}
			if metadata, err := store.NativeIdentityMetadata(); err != nil || string(metadata) != "null" {
				t.Fatal("identity did not clear", err, string(metadata))
			}
			if err := store.Close(); err != nil {
				t.Fatal(err)
			}
			reopened, err := Open(root, Options{Key: key})
			if err != nil {
				t.Fatal(err)
			}
			defer reopened.Close()
			if metadata, err := reopened.NativeIdentityMetadata(); err != nil || string(metadata) != "null" {
				t.Fatal("identity persisted across Store handles", err)
			}
			if data, err := reopened.Get(signed); err != nil || !bytes.Contains(data, revision.Signature) {
				t.Fatal("signed revision needed private identity to read", err)
			}
		})
	}
}
