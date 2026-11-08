package syrnck

import (
	"context"
	"encoding/json"
	"errors"
	"runtime"
	"time"
)

var ErrWorkspaceClosed = errors.New("syrnck: workspace is closed")
var zeroWorkspaceHandle nativeWorkspaceHandle

// WorkspaceOptions' zero value selects native notifications with polling fallback.
type WorkspaceOptions struct {
	// Name selects a registered native context; empty observes primary.
	Name                     string
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
	mu     contextMutex
	handle nativeWorkspaceHandle
}

func OpenWorkspace(directory, storage string, options WorkspaceOptions) (*Workspace, error) {
	return OpenWorkspaceContext(context.Background(), directory, storage, options)
}

// OpenWorkspaceContext can cancel native recursive registration. Both paths
// must exist; individual OS filesystem calls may finish before a stop check.
func OpenWorkspaceContext(ctx context.Context, directory, storage string, options WorkspaceOptions) (*Workspace, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	root, err := text(directory)
	if err != nil {
		return nil, err
	}
	store, err := text(storage)
	if err != nil {
		return nil, err
	}
	name, err := text(options.Name)
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
	var handle nativeWorkspaceHandle
	err = withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
		var failure error
		handle, failure = nativeWorkspaceOpen(root, store, name, &config, cancellation)
		return failure
	})
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

// Wait accepts 0..one minute. Context can interrupt native waits and recursive
// registration/recovery at cooperative checkpoints. Native cancellation
// invalidates continuity for a complete byte scan on the next Status.
// Close waits for the active call. A native result completed before cancellation
// was observed is returned normally, even if the context expires just afterwards.
func (w *Workspace) Wait(ctx context.Context, timeout time.Duration) (WorkspaceChanges, error) {
	var result WorkspaceChanges
	if timeout < 0 || timeout > time.Minute {
		return result, errors.New("syrnck: workspace wait must be in 0..one minute")
	}
	if err := w.mu.LockContext(ctx); err != nil {
		return result, err
	}
	defer w.mu.Unlock()
	if w.handle == zeroWorkspaceHandle {
		return result, ErrWorkspaceClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	milliseconds := uint32((timeout + time.Millisecond - 1) / time.Millisecond)
	var data []byte
	err := withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
		var failure error
		data, failure = nativeWorkspaceWait(w.handle, milliseconds, cancellation)
		return failure
	})
	if err != nil {
		return result, err
	}
	if err = json.Unmarshal(data, &result); err != nil {
		return WorkspaceChanges{}, err
	}
	return result, nil
}

// Status returns the canonical native status JSON (combined or working).
// It is equivalent to StatusContext with context.Background().
func (w *Workspace) Status(store *Store, selection WorkspaceSelection) (json.RawMessage, error) {
	return w.StatusContext(context.Background(), store, selection)
}

// StatusContext propagates cancellation through scanning, file hashing and
// metadata comparison. Individual filesystem calls/JSON encoding may finish
// before the next check. Failure invalidates continuity for full recovery.
func (w *Workspace) StatusContext(ctx context.Context, store *Store, selection WorkspaceSelection) (json.RawMessage, error) {
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
	if err := w.mu.LockContext(ctx); err != nil {
		return nil, err
	}
	defer w.mu.Unlock()
	if w.handle == zeroWorkspaceHandle {
		return nil, ErrWorkspaceClosed
	}
	var data []byte
	err := store.withContext(ctx, func() error {
		return withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
			var failure error
			data, failure = nativeWorkspaceStatus(w.handle, store.handle, selected, mode, cancellation)
			return failure
		})
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
