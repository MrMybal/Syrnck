package syrnck

import (
	"context"
	"encoding/json"
	"errors"
	"runtime"
	"sync"
	"time"
)

var ErrWorkspaceClosed = errors.New("syrnck: workspace is closed")
var zeroWorkspaceHandle nativeWorkspaceHandle

// WorkspaceOptions' zero value selects native notifications with polling fallback.
type WorkspaceOptions struct {
	Polling                  bool
	MaxPaths, MaxDirectories uint32
}

// WorkspaceChanges contains advisory hints, also retained by Workspace until a
// successful Status. Ignoring a Wait result never loses the native batch.
type WorkspaceChanges struct {
	Version  uint32   `json:"version"`
	Backend  string   `json:"backend"`
	Paths    []string `json:"paths"`
	FullScan bool     `json:"full_scan"`
	Overflow bool     `json:"overflow"`
	Polling  bool     `json:"polling"`
}

func (c WorkspaceChanges) Changed() bool {
	return c.FullScan || c.Overflow || c.Polling || len(c.Paths) != 0
}

// WorkspaceSelection's zero value follows the selected native HEAD branch.
// Revision excludes Branch and Combined; Combined separates staging and work.
type WorkspaceSelection struct {
	Branch   string
	Revision string
	Combined bool
}

// Workspace must not be copied. Calls and Close are serialized. It owns no
// Store and holds no Store lock. Arm it before the first Status, reopen the
// Store for each query, and close that Store before waiting again. Schedule
// periodic Invalidate calls for complete audits independently of notifications.
type Workspace struct {
	mu     sync.Mutex
	handle nativeWorkspaceHandle
}

func OpenWorkspace(directory, storage string, options WorkspaceOptions) (*Workspace, error) {
	root, err := text(directory)
	if err != nil {
		return nil, err
	}
	store, err := text(storage)
	if err != nil {
		return nil, err
	}
	if options.MaxPaths == 0 {
		options.MaxPaths = 4096
	}
	if options.MaxDirectories == 0 {
		options.MaxDirectories = 65536
	}
	config := [3]uint32{0, options.MaxPaths, options.MaxDirectories}
	if options.Polling {
		config[0] = 1
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	handle, err := nativeWorkspaceOpen(root, store, &config)
	if err != nil {
		return nil, err
	}
	return &Workspace{handle: handle}, nil
}

func (w *Workspace) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.handle == zeroWorkspaceHandle {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	err := nativeWorkspaceClose(w.handle)
	w.handle = zeroWorkspaceHandle
	return err
}

func (w *Workspace) Invalidate() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.handle == zeroWorkspaceHandle {
		return ErrWorkspaceClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	return nativeWorkspaceInvalidate(w.handle)
}

// Wait accepts 0..one minute. Context is checked between native waits of at most
// 50 ms; a native registration/recovery already underway must finish first.
// A cancelled result does not discard internally retained hints. Close waits
// for the active call; cancel the context to finish a long idle Wait early.
func (w *Workspace) Wait(ctx context.Context, timeout time.Duration) (WorkspaceChanges, error) {
	var result WorkspaceChanges
	if timeout < 0 || timeout > time.Minute {
		return result, errors.New("syrnck: workspace wait must be in 0..one minute")
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.handle == zeroWorkspaceHandle {
		return result, ErrWorkspaceClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	deadline := time.Now().Add(timeout)
	for {
		if err := ctx.Err(); err != nil {
			return WorkspaceChanges{}, err
		}
		remaining := time.Until(deadline)
		var tick uint32
		if remaining > 0 {
			tick = uint32((min(remaining, 50*time.Millisecond) + time.Millisecond - 1) / time.Millisecond)
		}
		data, err := nativeWorkspaceWait(w.handle, tick)
		if err != nil {
			return WorkspaceChanges{}, err
		}
		if err = json.Unmarshal(data, &result); err != nil {
			return WorkspaceChanges{}, err
		}
		if err = ctx.Err(); err != nil {
			return WorkspaceChanges{}, err
		}
		if result.FullScan || result.Overflow || len(result.Paths) != 0 || !time.Now().Before(deadline) {
			return result, nil
		}
		// Polling is returned at the caller's deadline, rather than every 50 ms.
	}
}

// Status returns the canonical native status JSON (combined or working).
// Status and native registration are not cancellable inside an ABI call yet.
// Failed queries invalidate continuity; the next success reads all file bytes.
func (w *Workspace) Status(store *Store, selection WorkspaceSelection) (json.RawMessage, error) {
	if store == nil {
		return nil, errors.New("syrnck: nil store")
	}
	var mode uint32
	selector := selection.Branch
	if selection.Revision != "" {
		if selection.Branch != "" || selection.Combined {
			return nil, errors.New("syrnck: revision excludes branch and combined status")
		}
		selector, mode = selection.Revision, 1
	} else if selection.Combined {
		mode = 2
	}
	var selected []byte
	if selector != "" {
		var err error
		selected, err = text(selector)
		if err != nil {
			return nil, err
		}
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.handle == zeroWorkspaceHandle {
		return nil, ErrWorkspaceClosed
	}
	var data []byte
	err := store.with(func() error {
		var failure error
		data, failure = nativeWorkspaceStatus(w.handle, store.handle, selected, mode)
		return failure
	})
	if err != nil {
		// Store.with can fail before entering the ABI (for example, Close).
		// Preserve the same full-recovery rule for every attempted query.
		runtime.LockOSThread()
		_ = nativeWorkspaceInvalidate(w.handle)
		runtime.UnlockOSThread()
	}
	return json.RawMessage(data), err
}
