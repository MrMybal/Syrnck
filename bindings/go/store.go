// Package syrnck exposes the C++ block engine through its C ABI.
// Windows uses the system DLL loader; Linux/macOS use cgo.
package syrnck

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"runtime"
	"strings"
	"sync/atomic"
	"unicode/utf8"
)

const MaxObjectSize = 16 * 1024 * 1024

var ErrClosed = errors.New("syrnck: store is closed")
var sequence atomic.Uint64
var zeroHandle nativeHandle

type ObjectKey struct {
	Kind string `json:"kind"`
	ID   string `json:"id"`
}

type Options struct {
	Create bool
	Key    []byte // absent or exactly 32 bytes; only used during Open
}

// Chunking's zero value selects the engine's default FastCDC2020 settings.
// For fixed blocks use Fixed=true and Min=Average=Max=size.
type Chunking struct {
	Fixed             bool
	Min, Average, Max uint32
}

// Store must not be copied. Calls and Close are serialized. Close explicitly:
// the process-wide exclusive store lock is held for the handle's lifetime.
type Store struct {
	mu     contextMutex
	handle nativeHandle
	order  uint64
}

func text(s string) ([]byte, error) {
	if !utf8.ValidString(s) || strings.IndexByte(s, 0) >= 0 {
		return nil, errors.New("syrnck: expected UTF-8 without NUL")
	}
	return append([]byte(s), 0), nil
}

func keyText(k ObjectKey) ([]byte, []byte, error) {
	kind, err := text(k.Kind)
	if err != nil {
		return nil, nil, err
	}
	id, err := text(k.ID)
	return kind, id, err
}

func status(code int) error {
	if code == 3 {
		return errNativeCancelled
	}
	if code == 0 {
		return nil
	}
	return fmt.Errorf("syrnck: %s (status %d)", nativeError(), code)
}

func Open(path string, options Options) (*Store, error) {
	p, err := text(path)
	if err != nil {
		return nil, err
	}
	if len(options.Key) != 0 && len(options.Key) != 32 {
		return nil, errors.New("syrnck: key must contain exactly 32 bytes")
	}
	// The C error is thread-local. Read it before allowing goroutine migration.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	handle, err := nativeOpen(p, options.Create, options.Key)
	if err != nil {
		return nil, err
	}
	return &Store{handle: handle, order: sequence.Add(1)}, nil
}

func (s *Store) with(operation func() error) error {
	return s.withContext(context.Background(), operation)
}
func (s *Store) withContext(ctx context.Context, operation func() error) error {
	if err := s.mu.LockContext(ctx); err != nil {
		return err
	}
	defer s.mu.Unlock()
	if s.handle == zeroHandle {
		return ErrClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	return operation()
}

func (s *Store) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.handle == zeroHandle {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	err := nativeClose(s.handle)
	s.handle = zeroHandle
	return err
}

// UseWorkspace selects a registered native context on this handle. Empty selects
// primary. It does not select a context for another Store handle or Workspace.
func (s *Store) UseWorkspace(name string) error {
	value, err := text(name)
	if err != nil {
		return err
	}
	return s.with(func() error { return nativeUseWorkspace(s.handle, value) })
}

func (s *Store) Ingest(path string, chunking Chunking) (id string, err error) {
	p, err := text(path)
	if err != nil {
		return "", err
	}
	config := chunkingConfig(chunking)
	err = s.with(func() error { id, err = nativeIngest(s.handle, p, config); return err })
	return
}

// VerifyFile checks every chunk and the whole file hash without a disk export.
func (s *Store) VerifyFile(id string) error {
	i, err := text(id)
	if err != nil {
		return err
	}
	return s.with(func() error { return nativeVerifyFile(s.handle, i) })
}

func (s *Store) Restore(id, destination string) error {
	i, err := text(id)
	if err != nil {
		return err
	}
	p, err := text(destination)
	if err != nil {
		return err
	}
	return s.with(func() error { return nativeRestore(s.handle, i, p) })
}

// ReadRange verifies selected chunks and returns exactly length bytes, capped at
// MaxObjectSize per call. Whole-file ranges also verify the complete file hash.
func (s *Store) ReadRange(id string, offset, length uint64) (data []byte, err error) {
	i, err := text(id)
	if err != nil {
		return nil, err
	}
	if length > MaxObjectSize {
		return nil, errors.New("syrnck: buffered file range exceeds 16 MiB")
	}
	err = s.with(func() error { data, err = nativeRange(s.handle, i, offset, length); return err })
	return
}

// RestoreRange streams an exact range to a new file, preserving existing paths.
func (s *Store) RestoreRange(id, destination string, offset, length uint64) error {
	i, err := text(id)
	if err != nil {
		return err
	}
	p, err := text(destination)
	if err != nil {
		return err
	}
	return s.with(func() error { return nativeRestoreRange(s.handle, i, p, offset, length) })
}

type FileBlock struct {
	ID           string  `json:"id"`
	Offset       uint64  `json:"offset"`
	Size         uint64  `json:"size"`
	SourceOffset *uint64 `json:"source_offset"`
}
type NeededChunk struct {
	ID   string `json:"id"`
	Size uint64 `json:"size"`
}
type FileComparison struct {
	Version        uint32        `json:"version"`
	Source         string        `json:"source"`
	Target         string        `json:"target"`
	SourceSize     uint64        `json:"source_size"`
	TargetSize     uint64        `json:"target_size"`
	SameContent    bool          `json:"same_content"`
	ReusedBytes    uint64        `json:"reused_bytes"`
	NewBytes       uint64        `json:"new_bytes"`
	UniqueNewBytes uint64        `json:"unique_new_bytes"`
	Blocks         []FileBlock   `json:"blocks"`
	NeededChunks   []NeededChunk `json:"needed_chunks"`
}

// CompareFiles creates a metadata-only recipe; it does not verify source bytes
// or imply chunk availability. Verify imported chunks and the finished file.
func (s *Store) CompareFiles(source, target string) (result FileComparison, err error) {
	a, err := text(source)
	if err != nil {
		return result, err
	}
	b, err := text(target)
	if err != nil {
		return result, err
	}
	err = s.with(func() error {
		data, e := nativeCompare(s.handle, a, b)
		if e != nil {
			return e
		}
		return json.Unmarshal(data[:len(data)-1], &result)
	})
	return
}

func (s *Store) Has(k ObjectKey) (present bool, err error) {
	kind, id, err := keyText(k)
	if err != nil {
		return false, err
	}
	err = s.with(func() error { present, err = nativeHas(s.handle, kind, id); return err })
	return
}

func (s *Store) Get(k ObjectKey) (data []byte, err error) {
	kind, id, err := keyText(k)
	if err != nil {
		return nil, err
	}
	err = s.with(func() error { data, err = nativeRead(s.handle, kind, id, false); return err })
	return
}

// Import checks the hash and format. Pin only after importing the entire closure.
func (s *Store) Import(k ObjectKey, data []byte) (created bool, err error) {
	kind, id, err := keyText(k)
	if err != nil {
		return false, err
	}
	if len(data) > MaxObjectSize {
		return false, errors.New("syrnck: object too large")
	}
	err = s.with(func() error { created, err = nativeImport(s.handle, kind, id, data); return err })
	return
}

func closure(handle nativeHandle, k ObjectKey) ([]ObjectKey, error) {
	kind, id, err := keyText(k)
	if err != nil {
		return nil, err
	}
	data, err := nativeRead(handle, kind, id, true)
	if err != nil {
		return nil, err
	}
	var result []ObjectKey
	err = json.Unmarshal(data[:len(data)-1], &result)
	return result, err
}

func (s *Store) Closure(k ObjectKey) (result []ObjectKey, err error) {
	err = s.with(func() error { result, err = closure(s.handle, k); return err })
	return
}

func pin(handle nativeHandle, name string, k ObjectKey) error {
	n, err := text(name)
	if err != nil {
		return err
	}
	kind, id, err := keyText(k)
	if err != nil {
		return err
	}
	return nativePin(handle, n, kind, id)
}

func (s *Store) Pin(name string, k ObjectKey) error {
	return s.with(func() error { return pin(s.handle, name, k) })
}

func (s *Store) Unpin(name string) error {
	n, err := text(name)
	if err != nil {
		return err
	}
	return s.with(func() error { return nativeUnpin(s.handle, n) })
}

func (s *Store) Verify() (objects uint64, err error) {
	err = s.with(func() error { objects, err = nativeVerify(s.handle); return err })
	return
}

type TransferStats struct {
	Objects, Bytes, ReusedObjects uint64
}

// CopyMissing is a local integration harness, not a Syncthing network adapter.
// It copies verified plaintext objects one at a time, then publishes a pin after
// checking the entire file/revision closure. Existing objects enable retry.
// Cancellation is checked between objects; an in-progress native call completes.
// Both stores stay locked for the operation, preventing concurrent Close/GC.
func CopyMissing(ctx context.Context, source, destination *Store, root ObjectKey, ref string) (stats TransferStats, err error) {
	first, second := source, destination
	if first.order > second.order {
		first, second = second, first
	}
	first.mu.Lock()
	defer first.mu.Unlock()
	if first != second {
		second.mu.Lock()
		defer second.mu.Unlock()
	}
	if source.handle == zeroHandle || destination.handle == zeroHandle {
		return stats, ErrClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err = ctx.Err(); err != nil {
		return
	}
	keys, err := closure(source.handle, root)
	if err != nil {
		return stats, err
	}
	for _, k := range keys {
		if err = ctx.Err(); err != nil {
			return
		}
		kind, id, e := keyText(k)
		if e != nil {
			return stats, e
		}
		present, e := nativeHas(destination.handle, kind, id)
		if e != nil {
			return stats, e
		}
		if present {
			stats.ReusedObjects++
			continue
		}
		data, e := nativeRead(source.handle, kind, id, false)
		if e != nil {
			return stats, e
		}
		if _, e = nativeImport(destination.handle, kind, id, data); e != nil {
			return stats, e
		}
		stats.Objects++
		stats.Bytes += uint64(len(data))
	}
	if err = ctx.Err(); err == nil {
		err = pin(destination.handle, ref, root)
	}
	return
}
