package syncthing

import (
	"bytes"
	"context"
	"crypto/hmac"
	"encoding/base32"
	"encoding/json"
	"errors"
	"math"
	"strings"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

// CipherBlock retains one exact BEP ciphertext block as one or two native
// manifests. The second part handles a 16 MiB block plus its 40-byte envelope,
// without weakening the native buffered input/range bound.
type CipherBlock struct {
	Parts []string `json:"parts,omitempty"`
}

func catalogStateRef(state catalogState) string {
	name := catalogRef(state.Device, state.Folder)
	if state.Version == 2 {
		name = strings.Replace(name, "ffi/bep/", "ffi/bep-encrypted/", 1)
	}
	return name
}
func catalogMessage(version int) string {
	if version == 2 {
		return "Syrnck encrypted BEP folder catalog v1"
	}
	return "Syrnck BEP folder catalog v1"
}
func copyCatalogEntry(entry CatalogEntry) CatalogEntry {
	entry.Info = copyInfo(entry.Info)
	if entry.CipherBlocks != nil {
		blocks := make([]CipherBlock, len(entry.CipherBlocks))
		for i, block := range entry.CipherBlocks {
			blocks[i].Parts = append([]string(nil), block.Parts...)
		}
		entry.CipherBlocks = blocks
	}
	return entry
}
func encryptedNameValid(name string) bool {
	if len(name) < 34 || len(name) > 8192 || strings.ContainsAny(name, "\\\x00") {
		return false
	}
	parts := strings.Split(name, "/")
	if len(parts) < 3 || len(parts[0]) != 15 || !strings.HasSuffix(parts[0], ".syncthing-enc") || len(parts[1]) != 2 {
		return false
	}
	encoded := parts[0][:1] + strings.Join(parts[1:], "")
	for i, part := range parts[2:] {
		if len(part) == 0 || len(part) > 200 || i < len(parts)-3 && len(part) != 200 {
			return false
		}
	}
	decoded, err := base32.HexEncoding.WithPadding(base32.NoPadding).DecodeString(encoded)
	return err == nil && len(decoded) > 16 && base32.HexEncoding.WithPadding(base32.NoPadding).EncodeToString(decoded) == encoded
}
func validateEncryptedInfo(info protocol.FileInfo) error {
	if !encryptedNameValid(info.Name) || info.Sequence <= 0 || info.Size < 0 || info.LocalFlags != 0 || info.New ||
		info.EncryptionTrailerSize != 0 || len(info.Encrypted) < 40 || len(info.Encrypted) > syrnck.MaxObjectSize ||
		info.IsInvalid() || len(info.SymlinkTarget) != 0 || info.ModifiedBy != 0 || info.ModifiedNs != 0 ||
		info.ModifiedS != 1234567890 || info.Permissions != 0o644 || info.NoPermissions ||
		info.Platform != (protocol.PlatformData{}) || len(info.PreviousBlocksHash) != 0 ||
		len(info.Version.Counters) != 1 || info.Version.Counters[0].ID != 1 || info.Version.Counters[0].Value == 0 {
		return errors.New("syrnck BEP: invalid encrypted file metadata")
	}
	if info.Type != protocol.FileInfoTypeFile && info.Type != protocol.FileInfoTypeDirectory {
		return errors.New("syrnck BEP: invalid encrypted file type")
	}
	if info.Deleted || info.Type == protocol.FileInfoTypeDirectory {
		if info.Size == 0 && len(info.Blocks) == 0 && len(info.BlocksHash) == 0 {
			return nil
		}
		return errors.New("syrnck BEP: invalid encrypted metadata-only entry")
	}
	if !blockSizeValid(info.BlockSize()-40) || len(info.Blocks) == 0 || len(info.Blocks) > MaxBlocks {
		return errors.New("syrnck BEP: invalid encrypted block layout")
	}
	var offset int64
	for i, block := range info.Blocks {
		if block.Offset != offset || block.Size < 1064 || block.Size > info.BlockSize() || len(block.Hash) != 48 ||
			(i+1 < len(info.Blocks) && block.Size != info.BlockSize()) || int64(block.Size) > info.Size-offset {
			return errors.New("syrnck BEP: invalid encrypted block layout")
		}
		offset += int64(block.Size)
	}
	if offset != info.Size || len(info.BlocksHash) != 0 && !bytes.Equal(info.BlocksHash, protocol.BlocksHash(info.Blocks)) {
		return errors.New("syrnck BEP: invalid encrypted block layout")
	}
	return nil
}
func validateCatalogInfoMode(state catalogState, info protocol.FileInfo) error {
	if state.Version == 2 {
		return validateEncryptedInfo(info)
	}
	return validateCatalogInfo(state.Folder, info)
}
func validateCatalogEntry(state catalogState, entry CatalogEntry, local bool) error {
	if err := validateCatalogInfoMode(state, entry.Info); err != nil {
		return err
	}
	needsData := !entry.Info.Deleted && entry.Info.Type == protocol.FileInfoTypeFile
	if state.Version == 1 {
		if entry.Origin != "" || len(entry.CipherBlocks) != 0 || entry.Manifest != "" && (!needsData || !validManifest(entry.Manifest)) || local && needsData && entry.Manifest == "" {
			return errors.New("syrnck BEP: invalid catalog manifest binding")
		}
		return nil
	}
	if entry.Manifest != "" || !needsData && len(entry.CipherBlocks) != 0 || needsData && len(entry.CipherBlocks) != len(entry.Info.Blocks) {
		return errors.New("syrnck BEP: invalid encrypted data binding")
	}
	if local {
		found := false
		for _, peer := range state.Peers {
			found = found || peer.Device == entry.Origin
		}
		if !found || !encryptedEntryReady(entry) {
			return errors.New("syrnck BEP: invalid encrypted publication origin")
		}
	} else if entry.Origin != "" {
		return errors.New("syrnck BEP: invalid encrypted publication origin")
	}
	for i, block := range entry.CipherBlocks {
		if len(block.Parts) == 0 {
			continue
		}
		expected := (entry.Info.Blocks[i].Size + syrnck.MaxObjectSize - 1) / syrnck.MaxObjectSize
		if len(block.Parts) != expected {
			return errors.New("syrnck BEP: invalid encrypted data binding")
		}
		for _, manifest := range block.Parts {
			if !validManifest(manifest) {
				return errors.New("syrnck BEP: invalid encrypted data binding")
			}
		}
	}
	return nil
}
func verifyCatalogEntryData(ctx context.Context, store *syrnck.Store, state catalogState, entry CatalogEntry) error {
	if state.Version == 1 {
		if entry.Manifest != "" {
			return verifyCatalogData(ctx, store, state.Folder, entry.Info, entry.Manifest)
		}
		return nil
	}
	for number, block := range entry.CipherBlocks {
		left := entry.Info.Blocks[number].Size
		for _, id := range block.Parts {
			data, err := store.Get(syrnck.ObjectKey{Kind: "manifest", ID: id})
			if err != nil {
				return err
			}
			var metadata struct {
				Size uint64 `json:"size"`
			}
			if err := json.Unmarshal(data, &metadata); err != nil {
				return err
			}
			length := min(left, syrnck.MaxObjectSize)
			if metadata.Size != uint64(length) {
				return errors.New("syrnck BEP: encrypted block manifest size differs")
			}
			if err := ctx.Err(); err != nil {
				return err
			}
			if err := store.VerifyFile(id); err != nil {
				return err
			}
			left -= length
		}
	}
	return nil
}
func encryptedEntryReady(entry CatalogEntry) bool {
	if entry.Info.Deleted || entry.Info.Type != protocol.FileInfoTypeFile {
		return true
	}
	if len(entry.CipherBlocks) != len(entry.Info.Blocks) {
		return false
	}
	for _, block := range entry.CipherBlocks {
		if len(block.Parts) == 0 {
			return false
		}
	}
	return true
}
func sameEncryptedPublicInfo(a, b protocol.FileInfo) bool {
	a.Sequence, b.Sequence = 0, 0
	a.Encrypted, b.Encrypted = nil, nil
	a.BlocksHash, b.BlocksHash = nil, nil
	x, _ := json.Marshal(a)
	y, _ := json.Marshal(b)
	return bytes.Equal(x, y)
}
func sameCipherToken(a, b protocol.BlockInfo) bool {
	return a.Offset == b.Offset && a.Size == b.Size && bytes.Equal(a.Hash, b.Hash)
}
func reuseCipherBlocks(next *CatalogEntry, prior CatalogEntry) {
	if next.Info.Name != prior.Info.Name || len(next.CipherBlocks) == 0 {
		return
	}
	for i, block := range next.Info.Blocks {
		if i < len(prior.Info.Blocks) && i < len(prior.CipherBlocks) && sameCipherToken(block, prior.Info.Blocks[i]) {
			next.CipherBlocks[i].Parts = append([]string(nil), prior.CipherBlocks[i].Parts...)
		}
	}
}
func (c *Catalog) bindEncryptedToken(token []byte) error {
	if c.state.Version != 2 || len(token) != len(c.state.Folder)+25 {
		return errors.New("syrnck BEP: invalid encrypted folder token")
	}
	if len(c.state.PasswordToken) != 0 {
		if !hmac.Equal(c.state.PasswordToken, token) {
			return errors.New("syrnck BEP: encrypted receiver password token changed")
		}
		return nil
	}
	state := cloneCatalogState(c.state)
	state.PasswordToken = bytes.Clone(token)
	return c.replace(state)
}

// Reconcile only complete versions. Equal opaque clocks cannot reveal private
// causality: use a stable source-device tie break while retaining every peer's
// full version in the parent-linked native catalog history.
func (c *Catalog) reconcileEncrypted() error {
	if c.state.Version != 2 {
		return errors.New("syrnck BEP: encrypted catalog required")
	}
	state := cloneCatalogState(c.state)
	entries := make(map[string]CatalogEntry, len(state.Files))
	for _, entry := range state.Files {
		entries[entry.Info.Name] = entry
	}
	for _, peer := range state.Peers {
		for _, entry := range peer.Files {
			if !encryptedEntryReady(entry) {
				continue
			}
			old, found := entries[entry.Info.Name]
			if found {
				comparison := entry.Info.Version.Compare(old.Info.Version)
				if comparison == protocol.Lesser || comparison == protocol.Equal && peer.Device > old.Origin {
					continue
				}
				if comparison == protocol.Equal && peer.Device == old.Origin && sameEncryptedPublicInfo(entry.Info, old.Info) {
					continue
				}
			}
			if state.Sequence == math.MaxInt64 {
				return errors.New("syrnck BEP: local sequence overflow")
			}
			state.Sequence++
			entry = copyCatalogEntry(entry)
			entry.Info.Sequence = state.Sequence
			entry.Origin = peer.Device
			entries[entry.Info.Name] = entry
		}
	}
	state.Files = sortedCatalogEntries(entries)
	return c.replace(state)
}
