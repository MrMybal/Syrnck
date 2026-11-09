package syncthing

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"

	syrnck "syrnck.local/bindings/go"
)

func authoredFolderOptions(options FolderOptions, name string, seedByte byte, available *atomic.Bool) FolderOptions {
	open := options.OpenStore
	options.OpenStore = func(ctx context.Context) (*syrnck.Store, error) {
		store, err := open(ctx)
		if err != nil {
			return nil, err
		}
		var seed []byte
		if available.Load() {
			seed = bytes.Repeat([]byte{seedByte}, 32)
		}
		defer clear(seed)
		if err := store.ConfigureNativeIdentity(syrnck.NativeIdentity{Name: name, Email: "bep@example.invalid", TimezoneOffset: 60}, syrnck.NativeIdentityOptions{Seed: seed, RequireSignature: true}); err != nil {
			store.Close()
			return nil, err
		}
		return store, nil
	}
	return options
}

func TestFolderSignedCandidatesRecoverWithoutPrivateKey(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		for _, phase := range []string{"before", "after"} {
			t.Run(fmt.Sprintf("%v/%s", encrypted, phase), func(t *testing.T) {
				ctx := context.Background()
				root := tempDirectory(t)
				leftID, rightID := folderProcessIDs()
				suffix := map[bool]string{false: "plain", true: "encrypted"}[encrypted]
				var available atomic.Bool
				available.Store(true)
				leftOptions := authoredFolderOptions(folderTestOptions(filepath.Join(root, "left-"+suffix), leftID, rightID, encrypted), "Source", 31, &available)
				rightOptions := authoredFolderOptions(folderTestOptions(filepath.Join(root, "right-"+suffix), rightID, leftID, encrypted), "Receiver", 32, &available)
				left := createTestFolder(t, leftOptions, map[string][]byte{"asset": []byte("signed incoming data")})
				right := createTestFolder(t, rightOptions, nil)
				prepareFolderApplication(t, left, right, "asset")
				var candidate string
				public := ed25519.NewKeyFromSeed(bytes.Repeat([]byte{32}, 32)).Public().(ed25519.PublicKey)
				assertAuthorship := func(store *syrnck.Store, id string) error {
					data, err := store.Get(syrnck.ObjectKey{Kind: "revision", ID: id})
					if err != nil {
						return err
					}
					var revision struct {
						Version int `json:"version"`
						Author  struct {
							Name string `json:"name"`
						} `json:"author"`
						Signature struct {
							PublicKey string `json:"public_key"`
						} `json:"signature"`
					}
					if err := json.Unmarshal(data, &revision); err != nil {
						return err
					}
					if revision.Version != 2 || revision.Author.Name != "Receiver" || revision.Signature.PublicKey != hex.EncodeToString(public) {
						t.Fatal("incoming candidate lacked local native authorship/signature", string(data))
					}
					return nil
				}
				if err := right.withStore(ctx, func(store *syrnck.Store) error {
					loaded, err := right.load(ctx, store)
					if err != nil {
						return err
					}
					candidate = loaded.pending.Candidate
					return assertAuthorship(store, candidate)
				}); err != nil {
					t.Fatal(err)
				}
				if err := right.Close(); err != nil {
					t.Fatal(err)
				}
				terminateFolderAtCheckpoint(t, rightOptions, phase)
				available.Store(false)
				reopened, err := NewFolder(ctx, rightOptions)
				if err != nil {
					t.Fatal("prepared signed operation required private key", err)
				}
				defer reopened.Close()
				if _, err := reopened.Refresh(ctx); err != nil {
					t.Fatal("signed recovery required private key", err)
				}
				data, err := os.ReadFile(filepath.Join(rightOptions.Directory, "asset"))
				if err != nil || string(data) != "signed incoming data" {
					t.Fatal("signed candidate was not applied", err)
				}
				if err := os.WriteFile(filepath.Join(rightOptions.Directory, "asset"), []byte("new local edit"), 0600); err != nil {
					t.Fatal(err)
				}
				if err := reopened.Invalidate(); err != nil {
					t.Fatal(err)
				}
				if _, err := reopened.Refresh(ctx); err == nil {
					t.Fatal("missing required signer silently made an unsigned capture")
				}
				if err := reopened.withStore(ctx, func(store *syrnck.Store) error {
					loaded, err := reopened.load(ctx, store)
					if err != nil {
						return err
					}
					if loaded.native.Revision.ID != candidate {
						t.Fatal("failed signing changed native HEAD")
					}
					return nil
				}); err != nil {
					t.Fatal(err)
				}
				available.Store(true)
				if _, err := reopened.Refresh(ctx); err != nil {
					t.Fatal("restored signer did not capture edit", err)
				}
				if err := reopened.withStore(ctx, func(store *syrnck.Store) error {
					loaded, err := reopened.load(ctx, store)
					if err != nil {
						return err
					}
					if loaded.native.Revision.ID == candidate {
						t.Fatal("new signed capture did not advance")
					}
					return assertAuthorship(store, loaded.native.Revision.ID)
				}); err != nil {
					t.Fatal(err)
				}
			})
		}
	}
}
