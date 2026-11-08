package syncthing

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"fmt"

	"github.com/syncthing/syncthing/lib/protocol"
	syrnck "syrnck.local/bindings/go"
)

// DownloadOptions separates network waiting from exclusive native Store handles.
// ReadReuse serves exact blocks of ReuseIndex and may acquire a short-lived
// Store for each call. A different block size simply disables reuse. All returned
// data, including reusable data, is verified against the incoming SHA-256.
type DownloadOptions struct {
	ReuseIndex      *protocol.FileInfo
	ReadReuse       RequestFunc
	ResumeDirectory string
}

// StagedFile is a caller-serialized, complete verified download. It owns its
// private temporary file until Close; a caller-provided resume directory stays
// caller-owned. It holds no open file, native Store or network connection.
type StagedFile struct {
	folder, path string
	info         protocol.FileInfo
	stats        FetchStats
	cleanup      func()
	closed       bool
}

func (f *StagedFile) Info() protocol.FileInfo { return copyInfo(f.info) }
func (f *StagedFile) Stats() FetchStats       { return f.stats }
func (f *StagedFile) Close() error {
	if !f.closed {
		f.closed = true
		f.cleanup()
	}
	return nil
}

// Prepare imports an unpinned immutable file. Attach and Commit its manifest
// before releasing Store to GC. Private bytes are revalidated after ingestion,
// so a changed staging file cannot be bound to the announced catalog version.
func (f *StagedFile) Prepare(ctx context.Context, store *syrnck.Store) (FetchResult, error) {
	return f.ingest(ctx, store, false)
}

func (f *StagedFile) ingest(ctx context.Context, store *syrnck.Store, retained bool) (result FetchResult, err error) {
	result.Stats = f.stats
	if f.closed || store == nil {
		return result, errors.New("syrnck BEP: staged file closed or Store missing")
	}
	if err = ctx.Err(); err != nil {
		return result, err
	}
	blockSize := uint32(f.info.BlockSize())
	chunking := syrnck.Chunking{Fixed: true, Min: blockSize, Average: blockSize, Max: blockSize}
	if retained {
		result.Manifest, err = store.Ingest(f.path, chunking)
	} else {
		result.Manifest, err = store.PrepareFileContext(ctx, f.path, chunking)
	}
	if err != nil {
		return result, fmt.Errorf("syrnck BEP ingest: %w", err)
	}
	if err = verifyCatalogData(ctx, store, f.folder, f.info, result.Manifest); err != nil {
		return result, err
	}
	return result, ctx.Err()
}

// DownloadFile downloads blocks to private staging without opening a native
// Store. Call Prepare while briefly holding Store, attach the exact persisted
// peer version, Commit and close Store. Always close the returned StagedFile.
func DownloadFile(ctx context.Context, folder string, incoming protocol.FileInfo, request RequestFunc, options DownloadOptions) (*StagedFile, error) {
	file, _, err := downloadFile(ctx, folder, incoming, request, options)
	return file, err
}

func downloadFile(ctx context.Context, folder string, incoming protocol.FileInfo, request RequestFunc, options DownloadOptions) (result *StagedFile, stats FetchStats, err error) {
	incoming.Name = protocolName(incoming.Name)
	if err = validateIndex(folder, incoming); err != nil {
		return nil, stats, err
	}
	info := copyInfo(incoming)
	if err = ctx.Err(); err != nil {
		return nil, stats, err
	}
	reusable := make(map[blockKey]protocol.BlockInfo)
	var reuseName string
	if options.ReuseIndex != nil {
		reuse := copyInfo(*options.ReuseIndex)
		reuse.Name = protocolName(reuse.Name)
		if err = validateIndex(folder, reuse); err != nil {
			return nil, stats, err
		}
		if options.ReadReuse == nil {
			return nil, stats, errors.New("syrnck BEP: reuse reader required")
		}
		reuseName = reuse.Name
		if reuse.BlockSize() == info.BlockSize() {
			for _, block := range reuse.Blocks {
				reusable[identity(block)] = block
			}
		}
	}
	file, cleanup, err := stage(folder, info, options.ResumeDirectory)
	if err != nil {
		return nil, stats, err
	}
	accepted := false
	defer func() {
		_ = file.Close()
		if !accepted {
			cleanup()
		}
	}()
	if err = file.Truncate(info.Size); err != nil {
		return nil, stats, err
	}
	for number, block := range info.Blocks {
		if err = ctx.Err(); err != nil {
			return nil, stats, err
		}
		data := make([]byte, block.Size)
		_, readErr := file.ReadAt(data, block.Offset)
		digest := sha256.Sum256(data)
		if (readErr == nil || block.Size == 0) && bytes.Equal(digest[:], block.Hash) {
			stats.StagedBytes += uint64(block.Size)
			continue
		}
		if prior, found := reusable[identity(block)]; found {
			data, err = options.ReadReuse(ctx, &protocol.Request{Folder: folder, Name: reuseName, Offset: prior.Offset, Size: prior.Size,
				Hash: bytes.Clone(prior.Hash), BlockNo: int(prior.Offset / int64(info.BlockSize()))})
			if err != nil {
				return nil, stats, err
			}
			stats.ReusedBytes += uint64(block.Size)
		} else {
			if request == nil {
				return nil, stats, errors.New("syrnck BEP: missing block requester")
			}
			data, err = request(ctx, &protocol.Request{Folder: folder, Name: info.Name, Offset: block.Offset, Size: block.Size, Hash: bytes.Clone(block.Hash), BlockNo: number})
			if err != nil {
				return nil, stats, err
			}
			stats.Requests++
			stats.TransferredBytes += uint64(len(data))
		}
		if err = ctx.Err(); err != nil {
			return nil, stats, err
		}
		digest = sha256.Sum256(data)
		if len(data) != block.Size || !bytes.Equal(digest[:], block.Hash) {
			return nil, stats, errors.New("syrnck BEP: block hash or length mismatch")
		}
		if _, err = file.WriteAt(data, block.Offset); err != nil {
			return nil, stats, err
		}
		if (number+1)%16 == 0 {
			if err = file.Sync(); err != nil {
				return nil, stats, err
			}
		}
	}
	if err = file.Sync(); err != nil {
		return nil, stats, err
	}
	if err = file.Close(); err != nil {
		return nil, stats, err
	}
	if err = ctx.Err(); err != nil {
		return nil, stats, err
	}
	accepted = true
	return &StagedFile{folder: folder, path: file.Name(), info: info, stats: stats, cleanup: cleanup}, stats, nil
}
