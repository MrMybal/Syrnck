// Package syncthing adapts Syrnck manifests to Syncthing's fixed-block BEP data
// model. Device authentication, version vectors and workspace conflict handling
// remain the embedding application's responsibility.
package syncthing

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math"
	"os"
	"path/filepath"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/syncthing/syncthing/lib/protocol"
	"golang.org/x/text/unicode/norm"
	syrnck "syrnck.local/bindings/go"
)

const MaxBlocks = 65536

type File struct {
	store            *syrnck.Store
	manifest, folder string
	info             protocol.FileInfo
}

func nameValid(name string) bool {
	return len(name) <= 4096 && fs.ValidPath(name) && name != "." && !strings.ContainsAny(name, "\\\x00") &&
		norm.NFC.IsNormalString(name) && strings.IndexFunc(name, unicode.IsControl) < 0
}

func folderValid(folder string) bool {
	return folder != "" && len(folder) <= 256 && utf8.ValidString(folder) &&
		norm.NFC.IsNormalString(folder) && strings.IndexFunc(folder, unicode.IsControl) < 0
}

func blockSizeValid(size int) bool {
	return size >= protocol.MinBlockSize && size <= protocol.MaxBlockSize && size&(size-1) == 0
}

func copyInfo(info protocol.FileInfo) protocol.FileInfo {
	blocks := make([]protocol.BlockInfo, len(info.Blocks))
	for i, block := range info.Blocks {
		blocks[i] = block
		blocks[i].Hash = bytes.Clone(block.Hash)
	}
	info.Blocks = blocks
	info.BlocksHash = bytes.Clone(info.BlocksHash)
	info.Version.Counters = append([]protocol.Counter(nil), info.Version.Counters...)
	return info
}

// NewFile hashes verified ranges into the exact SHA-256 block descriptors used
// by BEP. Syrnck chunk boundaries and IDs remain independent. Zero selects the
// reference implementation's block-size policy.
func NewFile(ctx context.Context, store *syrnck.Store, manifest, folder, name string, blockSize int) (*File, error) {
	name = filepath.ToSlash(name)
	if store == nil || !nameValid(name) || !folderValid(folder) {
		return nil, errors.New("syrnck BEP: invalid file identity")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	data, err := store.Get(syrnck.ObjectKey{Kind: "manifest", ID: manifest})
	if err != nil {
		return nil, err
	}
	var metadata struct {
		Size uint64 `json:"size"`
	}
	if err := json.Unmarshal(data, &metadata); err != nil {
		return nil, err
	}
	if metadata.Size > math.MaxInt64 {
		return nil, errors.New("syrnck BEP: file size exceeds int64")
	}
	if blockSize == 0 {
		blockSize = protocol.BlockSize(int64(metadata.Size))
	}
	if !blockSizeValid(blockSize) {
		return nil, errors.New("syrnck BEP: invalid fixed block size")
	}
	count := (metadata.Size + uint64(blockSize) - 1) / uint64(blockSize)
	if count == 0 {
		count = 1
	}
	if count > MaxBlocks {
		return nil, errors.New("syrnck BEP: block index exceeds limit")
	}
	if err := store.VerifyFile(manifest); err != nil {
		return nil, err
	}
	info := protocol.FileInfo{Name: name, Size: int64(metadata.Size), Type: protocol.FileInfoTypeFile,
		RawBlockSize: int32(blockSize), NoPermissions: true, Permissions: 0666,
		Blocks: make([]protocol.BlockInfo, 0, int(count))}
	for i := uint64(0); i < count; i++ {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		offset := i * uint64(blockSize)
		length := min(uint64(blockSize), metadata.Size-offset)
		data, err := store.ReadRange(manifest, offset, length)
		if err != nil {
			return nil, err
		}
		hash := sha256.Sum256(data)
		info.Blocks = append(info.Blocks, protocol.BlockInfo{Offset: int64(offset), Size: len(data), Hash: bytes.Clone(hash[:])})
	}
	info.BlocksHash = protocol.BlocksHash(info.Blocks)
	return &File{store: store, manifest: manifest, folder: folder, info: info}, nil
}

func (f *File) Info() protocol.FileInfo { return copyInfo(f.info) }

// ReadRequest serves only exact advertised blocks of this immutable file. A
// missing optional hash is supported; a supplied hash must match the index.
func (f *File) ReadRequest(ctx context.Context, req *protocol.Request) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if req == nil || req.Folder != f.folder || filepath.ToSlash(req.Name) != f.info.Name || req.Offset < 0 ||
		req.Offset%int64(f.info.BlockSize()) != 0 {
		return nil, protocol.ErrNoSuchFile
	}
	number := req.Offset / int64(f.info.BlockSize())
	if number >= int64(len(f.info.Blocks)) || (req.BlockNo != 0 && int64(req.BlockNo) != number) {
		return nil, protocol.ErrNoSuchFile
	}
	block := f.info.Blocks[number]
	if req.Size != block.Size || (len(req.Hash) != 0 && !bytes.Equal(req.Hash, block.Hash)) {
		return nil, protocol.ErrInvalid
	}
	data, err := f.store.ReadRange(f.manifest, uint64(block.Offset), uint64(block.Size))
	if err != nil {
		return nil, err
	}
	hash := sha256.Sum256(data)
	if !bytes.Equal(hash[:], block.Hash) {
		return nil, protocol.ErrInvalid
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return data, nil
}

type RequestFunc func(context.Context, *protocol.Request) ([]byte, error)

type FetchOptions struct {
	// Existing immutable local file used to reuse moved/repeated BEP blocks.
	ReuseManifest string
	// Optional caller-owned, per-content staging directory. Matching verified
	// ranges survive cancellation; a different content index is refused.
	ResumeDirectory string
}

type FetchStats struct {
	Requests, TransferredBytes, ReusedBytes, StagedBytes uint64
}

type FetchResult struct {
	Manifest string
	Stats    FetchStats
}

func validateIndex(folder string, info protocol.FileInfo) error {
	if !folderValid(folder) || !nameValid(info.Name) || info.Size < 0 || info.Type != protocol.FileInfoTypeFile ||
		info.Deleted || info.IsInvalid() || len(info.Encrypted) != 0 || len(info.SymlinkTarget) != 0 ||
		!blockSizeValid(info.BlockSize()) || (info.RawBlockSize != 0 && int(info.RawBlockSize) != info.BlockSize()) ||
		len(info.Blocks) == 0 || len(info.Blocks) > MaxBlocks {
		return errors.New("syrnck BEP: invalid file index")
	}
	var offset int64
	for i, block := range info.Blocks {
		if block.Offset != offset || block.Size < 0 || block.Size > info.BlockSize() ||
			len(block.Hash) != sha256.Size || (i+1 < len(info.Blocks) && block.Size != info.BlockSize()) ||
			(block.Size == 0 && (info.Size != 0 || len(info.Blocks) != 1)) || int64(block.Size) > info.Size-offset {
			return errors.New("syrnck BEP: invalid block layout")
		}
		offset += int64(block.Size)
	}
	if offset != info.Size || (len(info.BlocksHash) != 0 && !bytes.Equal(info.BlocksHash, protocol.BlocksHash(info.Blocks))) {
		return errors.New("syrnck BEP: block index hash or length mismatch")
	}
	return nil
}

func stage(folder string, info protocol.FileInfo, directory string) (*os.File, func(), error) {
	if directory == "" {
		file, err := os.CreateTemp("", ".syrnck-bep-*")
		if err != nil {
			return nil, nil, err
		}
		return file, func() { _ = os.Remove(file.Name()) }, nil
	}
	metadata, err := json.Marshal(struct {
		Version      int
		Folder, Name string
		Size         int64
		BlockSize    int
		Hash         string
	}{1, folder, info.Name, info.Size, info.BlockSize(), hex.EncodeToString(protocol.BlocksHash(info.Blocks))})
	if err != nil {
		return nil, nil, err
	}
	if err := os.Mkdir(directory, 0700); err != nil && !errors.Is(err, fs.ErrExist) {
		return nil, nil, err
	}
	stat, err := os.Lstat(directory)
	if err != nil || !stat.IsDir() || stat.Mode()&os.ModeSymlink != 0 {
		return nil, nil, errors.New("syrnck BEP: invalid resume directory")
	}
	entries, err := os.ReadDir(directory)
	if err != nil {
		return nil, nil, err
	}
	for _, entry := range entries {
		if (entry.Name() != "state.json" && entry.Name() != "data") || !entry.Type().IsRegular() {
			return nil, nil, errors.New("syrnck BEP: foreign entry in resume directory")
		}
	}
	statePath := filepath.Join(directory, "state.json")
	var old []byte
	state, err := os.Open(statePath)
	if err == nil {
		old, err = io.ReadAll(io.LimitReader(state, 65537))
		_ = state.Close()
	}
	if errors.Is(err, fs.ErrNotExist) && len(entries) == 0 {
		state, createErr := os.OpenFile(statePath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if createErr != nil {
			return nil, nil, createErr
		}
		_, err = state.Write(metadata)
		if err == nil {
			err = state.Sync()
		}
		closeErr := state.Close()
		if err == nil {
			err = closeErr
		}
		if err != nil {
			return nil, nil, err
		}
	} else if err != nil || !bytes.Equal(old, metadata) {
		return nil, nil, errors.New("syrnck BEP: resume index changed or metadata is incomplete")
	}
	dataPath := filepath.Join(directory, "data")
	flags := os.O_RDWR | os.O_CREATE
	if _, err := os.Lstat(dataPath); errors.Is(err, fs.ErrNotExist) {
		flags |= os.O_EXCL
	} else if err != nil {
		return nil, nil, err
	}
	file, err := os.OpenFile(dataPath, flags, 0600)
	return file, func() {}, err
}

type blockKey struct {
	hash [32]byte
	size int
}

func identity(block protocol.BlockInfo) blockKey {
	var key blockKey
	copy(key.hash[:], block.Hash)
	key.size = block.Size
	return key
}

// Fetch validates SHA-256 blocks before writing private staging data. Only a
// complete verified file enters the engine, which computes its BLAKE3 manifest
// and retains it through the existing ffi/ root. No working file is replaced.
func Fetch(ctx context.Context, store *syrnck.Store, folder string, incoming protocol.FileInfo,
	request RequestFunc, options FetchOptions) (result FetchResult, err error) {
	if store == nil {
		return result, errors.New("syrnck BEP: nil destination store")
	}
	incoming.Name = filepath.ToSlash(incoming.Name)
	if err = validateIndex(folder, incoming); err != nil {
		return result, err
	}
	info := copyInfo(incoming)
	if err = ctx.Err(); err != nil {
		return result, err
	}
	reusable := map[blockKey]int64{}
	if options.ReuseManifest != "" {
		file, readErr := NewFile(ctx, store, options.ReuseManifest, folder, info.Name, info.BlockSize())
		if readErr != nil {
			return result, readErr
		}
		for _, block := range file.info.Blocks {
			reusable[identity(block)] = block.Offset
		}
	}
	file, cleanup, err := stage(folder, info, options.ResumeDirectory)
	if err != nil {
		return result, err
	}
	defer func() { _ = file.Close(); cleanup() }()
	if err = file.Truncate(info.Size); err != nil {
		return result, err
	}
	for number, block := range info.Blocks {
		if err = ctx.Err(); err != nil {
			return result, err
		}
		data := make([]byte, block.Size)
		_, readErr := file.ReadAt(data, block.Offset)
		digest := sha256.Sum256(data)
		if (readErr == nil || block.Size == 0) && bytes.Equal(digest[:], block.Hash) {
			result.Stats.StagedBytes += uint64(block.Size)
			continue
		}
		if offset, found := reusable[identity(block)]; found {
			data, err = store.ReadRange(options.ReuseManifest, uint64(offset), uint64(block.Size))
			if err != nil {
				return result, err
			}
			result.Stats.ReusedBytes += uint64(block.Size)
		} else {
			if request == nil {
				return result, errors.New("syrnck BEP: missing block requester")
			}
			data, err = request(ctx, &protocol.Request{Folder: folder, Name: info.Name, Offset: block.Offset,
				Size: block.Size, Hash: bytes.Clone(block.Hash), BlockNo: number})
			if err != nil {
				return result, err
			}
			result.Stats.Requests++
			result.Stats.TransferredBytes += uint64(len(data))
		}
		if err = ctx.Err(); err != nil {
			return result, err
		}
		digest = sha256.Sum256(data)
		if len(data) != block.Size || !bytes.Equal(digest[:], block.Hash) {
			return result, errors.New("syrnck BEP: block hash or length mismatch")
		}
		if _, err = file.WriteAt(data, block.Offset); err != nil {
			return result, err
		}
		if (number+1)%16 == 0 {
			if err = file.Sync(); err != nil {
				return result, err
			}
		}
	}
	if err = file.Sync(); err != nil {
		return result, err
	}
	if err = file.Close(); err != nil {
		return result, err
	}
	if err = ctx.Err(); err != nil {
		return result, err
	}
	blockSize := uint32(info.BlockSize())
	result.Manifest, err = store.Ingest(file.Name(), syrnck.Chunking{Fixed: true, Min: blockSize, Average: blockSize, Max: blockSize})
	if err != nil {
		return result, fmt.Errorf("syrnck BEP ingest: %w", err)
	}
	verified, checkErr := NewFile(ctx, store, result.Manifest, folder, info.Name, info.BlockSize())
	if checkErr != nil {
		return result, checkErr
	}
	if verified.info.Size != info.Size || !bytes.Equal(verified.info.BlocksHash, protocol.BlocksHash(info.Blocks)) {
		return result, errors.New("syrnck BEP: staged content changed during ingestion")
	}
	// Ingest is one synchronous ABI call. A late cancellation may retain this
	// complete manifest; return its ID alongside the cancellation error.
	return result, ctx.Err()
}
