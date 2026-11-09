package syncthing

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

var ErrFolderDownloadBusy = errors.New("syrnck BEP: another controller owns download staging")

type folderResume struct {
	directory string
	metadata  []byte
	lock      *os.File
}

func privateResumeDirectory(directory string, create bool) error {
	if create {
		if err := os.Mkdir(directory, 0700); err != nil && !errors.Is(err, os.ErrExist) {
			return err
		}
	}
	info, err := os.Lstat(directory)
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("syrnck BEP: resume path must be a private directory without links")
	}
	return nil
}

// acquireResume serializes a folder's private staging across processes. The
// operating-system lock holds no engine Store and is released on process exit.
func (f *Folder) acquireResume(download *folderDownload) (*folderResume, error) {
	storage, err := filepath.Abs(f.options.Storage)
	if err != nil {
		return nil, err
	}
	if err := privateResumeDirectory(storage, false); err != nil {
		return nil, err
	}
	owner := sha256.Sum256([]byte(f.prefix + "\x00" + f.options.Workspace))
	group := storage
	for _, part := range []string{"bep-downloads", "v1", hex.EncodeToString(owner[:])} {
		group = filepath.Join(group, part)
		if err := privateResumeDirectory(group, true); err != nil {
			return nil, err
		}
	}
	lockPath := filepath.Join(group, ".lock")
	if info, err := os.Lstat(lockPath); err == nil && !info.Mode().IsRegular() {
		return nil, errors.New("syrnck BEP: invalid download lock")
	} else if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	lock, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	opened, openErr := lock.Stat()
	current, statErr := os.Lstat(lockPath)
	if openErr != nil || statErr != nil || !current.Mode().IsRegular() || !os.SameFile(opened, current) {
		lock.Close()
		return nil, errors.New("syrnck BEP: download lock changed")
	}
	if err := lockDownload(lock); err != nil {
		lock.Close()
		return nil, fmt.Errorf("syrnck BEP download lock: %w", err)
	}
	metadata, err := stageMetadata(f.options.Catalog.Folder, download.Info)
	if err != nil {
		lock.Close()
		return nil, err
	}
	identity, err := json.Marshal(struct {
		Device  string
		IndexID uint64
		Content json.RawMessage
	}{download.Device.String(), download.IndexID, metadata})
	if err != nil {
		lock.Close()
		return nil, err
	}
	digest := sha256.Sum256(identity)
	return &folderResume{directory: filepath.Join(group, hex.EncodeToString(digest[:])), metadata: metadata, lock: lock}, nil
}

func (r *folderResume) Close() error {
	if r == nil || r.lock == nil {
		return nil
	}
	err := r.lock.Close()
	r.lock = nil
	return err
}

// retire removes only the known, matching pair after verified catalog retention.
// Never recursively remove staging: unknown contents require explicit inspection.
func (r *folderResume) retire() error {
	if err := privateResumeDirectory(r.directory, false); err != nil {
		return err
	}
	entries, err := os.ReadDir(r.directory)
	if err != nil {
		return err
	}
	if len(entries) != 2 {
		return errors.New("syrnck BEP: unexpected completed staging entries")
	}
	for _, entry := range entries {
		if (entry.Name() != "data" && entry.Name() != "state.json") || !entry.Type().IsRegular() {
			return errors.New("syrnck BEP: invalid completed staging entry")
		}
	}
	state, err := os.Open(filepath.Join(r.directory, "state.json"))
	if err != nil {
		return err
	}
	metadata, err := io.ReadAll(io.LimitReader(state, 65537))
	err = errors.Join(err, state.Close())
	if err != nil {
		return err
	}
	if !bytes.Equal(metadata, r.metadata) {
		return errors.New("syrnck BEP: completed staging identity changed")
	}
	for _, name := range []string{"data", "state.json"} {
		if err := os.Remove(filepath.Join(r.directory, name)); err != nil {
			return err
		}
	}
	return os.Remove(r.directory)
}
