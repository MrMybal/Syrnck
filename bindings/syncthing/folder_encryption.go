package syncthing

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"unicode/utf8"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

// Reused by one service's reconnects. The pinned upstream implementation owns
// password strings and cached derived keys; Go does not promise secure erasure.
type folderEncryption struct {
	folder, password string
	token            []byte
	keys             *protocol.KeyGenerator
}

func prepareFolderEncryption(folder *Folder, options *FolderSessionOptions) error {
	if !options.Untrusted {
		if options.EncryptionPassword != "" || options.encryption != nil {
			return errors.New("syrnck BEP: folder password requires an untrusted peer")
		}
		return nil
	}
	password := options.EncryptionPassword
	if len(password) == 0 || len(password) > 4096 || !utf8.ValidString(password) || strings.ContainsRune(password, 0) {
		return errors.New("syrnck BEP: untrusted peer requires a UTF-8 folder password of 1..4096 bytes without NUL")
	}
	if options.encryption == nil {
		keys := protocol.NewKeyGenerator()
		options.encryption = &folderEncryption{folder: folder.options.Catalog.Folder, password: password,
			keys: keys, token: protocol.PasswordToken(keys, folder.options.Catalog.Folder, password)}
	}
	if options.encryption.folder != folder.options.Catalog.Folder || options.encryption.password != password {
		return errors.New("syrnck BEP: encrypted folder session binding changed")
	}
	options.Device.keyGenerator = options.encryption.keys
	return nil
}

func checkFolderEncryption(options FolderSessionOptions, own, remote *protocol.Device) error {
	localToken, remoteToken := own.EncryptionPasswordToken, remote.EncryptionPasswordToken
	if !options.Untrusted {
		if len(localToken) != 0 || len(remoteToken) != 0 {
			return errors.New("syrnck BEP: invalid shared device generation or encryption agreement")
		}
		return nil
	}
	// Upstream generations can put the one token on either device. Requiring
	// exactly one matching token prevents plaintext fallback and inverted roles.
	if options.encryption == nil || (len(localToken) == 0) == (len(remoteToken) == 0) {
		return errors.New("syrnck BEP: untrusted peer did not confirm folder encryption")
	}
	token := remoteToken
	if len(token) == 0 {
		token = localToken
	}
	if !hmac.Equal(token, options.encryption.token) {
		return errors.New("syrnck BEP: untrusted peer folder password token differs")
	}
	return nil
}

// The upstream codec requests at least 1024 plaintext bytes before encrypting
// a padded short final block. Read only the exact announced original range;
// its existing offset/number/hash checks still apply. The codec adds padding.
func clipEncryptedPadding(entry CatalogEntry, request *protocol.Request) {
	if request.Size != 1024 || request.Offset < 0 || entry.Info.BlockSize() <= 0 ||
		request.Offset%int64(entry.Info.BlockSize()) != 0 {
		return
	}
	number := request.Offset / int64(entry.Info.BlockSize())
	if number < int64(len(entry.Info.Blocks)) && entry.Info.Blocks[number].Size < 1024 {
		request.Size = entry.Info.Blocks[number].Size
	}
}

type folderEncryptionBinding struct {
	Version   int    `json:"version"`
	Folder    string `json:"folder"`
	Local     string `json:"local"`
	Peer      string `json:"peer"`
	Transport string `json:"transport"`
	TokenHash string `json:"token_hash"`
}

// Once a peer receives encrypted data, a missing password or a changed secret
// must never silently reuse its catalog in plaintext after restarting. An
// explicit plaintext->encrypted upgrade is safe; downgrade/rekey needs migration.
func (f *Folder) bindEncryption(ctx context.Context, options FolderSessionOptions) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return errors.New("syrnck BEP: folder closed")
	}
	binding := folderEncryptionBinding{Version: 1, Folder: f.options.Catalog.Folder,
		Local: f.options.Catalog.LocalDevice.String(), Peer: options.Device.RemoteDevice.String(), Transport: "plain"}
	if options.Untrusted {
		if options.encryption == nil {
			return errors.New("syrnck BEP: encrypted folder session binding changed")
		}
		binding.Transport = "syncthing-untrusted-v1"
		digest := sha256.Sum256(options.encryption.token)
		binding.TokenHash = hex.EncodeToString(digest[:])
	}
	digest := sha256.Sum256([]byte(binding.Peer))
	name := f.prefix + "/encryption/" + hex.EncodeToString(digest[:])
	data, err := json.Marshal(binding)
	if err != nil {
		return err
	}
	return f.withStore(ctx, func(store *syrnck.Store) error {
		refs, err := store.RefsContext(ctx)
		if err != nil {
			return err
		}
		old := refKey(refs, name)
		if old != nil {
			if old.Kind != "chunk" {
				return errors.New("syrnck BEP: invalid folder encryption checkpoint")
			}
			previous, err := store.Get(*old)
			if err != nil {
				return err
			}
			if bytes.Equal(previous, data) {
				return nil
			}
			var saved folderEncryptionBinding
			if err := strictCatalogJSON(previous, &saved); err != nil {
				return err
			}
			expected := binding
			expected.Transport, expected.TokenHash = "plain", ""
			if !options.Untrusted || saved != expected {
				return errors.New("syrnck BEP: folder encryption checkpoint refuses downgrade or changed password")
			}
		}
		key, err := store.Put("chunk", data)
		if err != nil {
			return err
		}
		applied, err := store.CompareRefsContext(ctx, []syrnck.RefUpdate{{Name: name, Expected: old, Value: &key}})
		if err == nil && !applied {
			return ErrFolderChanged
		}
		return err
	})
}
