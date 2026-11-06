package syrnck

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

type workspaceReport struct {
	Clean   bool    `json:"clean"`
	Base    *string `json:"base"`
	Branch  string  `json:"branch"`
	Files   uint64  `json:"workspace_files"`
	Hashed  uint64  `json:"hashed_bytes"`
	Changes []struct {
		Path string `json:"path"`
	} `json:"changes"`
	Observation struct {
		Mode string `json:"mode"`
		Full bool   `json:"full_scan"`
	} `json:"observation"`
	Staged   json.RawMessage `json:"staged"`
	Unstaged json.RawMessage `json:"unstaged"`
}

func decodeWorkspace(t *testing.T, data []byte) workspaceReport {
	t.Helper()
	var result workspaceReport
	if err := json.Unmarshal(data, &result); err != nil {
		t.Fatal(err)
	}
	return result
}
func openWorkspaceTest(t *testing.T, root, storage string, options WorkspaceOptions) *Workspace {
	t.Helper()
	w, err := OpenWorkspace(root, storage, options)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := w.Close(); err != nil {
			t.Error(err)
		}
	})
	return w
}
func workspacePath(t *testing.T, w *Workspace, wanted string) WorkspaceChanges {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		batch, err := w.Wait(context.Background(), 25*time.Millisecond)
		if err != nil {
			t.Fatal(err)
		}
		if batch.Polling {
			return batch
		}
		for _, path := range batch.Paths {
			if path == wanted {
				return batch
			}
		}
	}
	t.Fatal("missing native path", wanted)
	return WorkspaceChanges{}
}
func TestWorkspaceObservation(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		for _, polling := range []bool{false, true} {
			t.Run(fmt.Sprintf("encrypted=%t/polling=%t", encrypted, polling), func(t *testing.T) {
				root := tempDirectory(t)
				storage := filepath.Join(root, ".syrnck")
				var key []byte
				if encrypted {
					key = bytes.Repeat([]byte{63}, 32)
				}
				s := openTest(t, storage, key)
				writeTest(t, filepath.Join(root, "large"), bytes.Repeat([]byte{'x'}, 4*1024*1024))
				writeTest(t, filepath.Join(root, "asset"), []byte("before"))
				if err := s.Close(); err != nil {
					t.Fatal(err)
				}
				w := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: polling})
				query := func(selection WorkspaceSelection) (workspaceReport, error) {
					store, err := Open(storage, Options{Key: key})
					if err != nil {
						return workspaceReport{}, err
					}
					defer store.Close()
					data, err := w.Status(store, selection)
					if err != nil {
						return workspaceReport{}, err
					}
					return decodeWorkspace(t, data), nil
				}
				initial, err := query(WorkspaceSelection{})
				if err != nil {
					t.Fatal(err)
				}
				if !initial.Observation.Full || initial.Hashed != 4*1024*1024+6 || initial.Files != 2 {
					t.Fatalf("initial full status differs: %+v", initial)
				}
				// The session remains alive while Store handles close/reopen.
				writeTest(t, filepath.Join(root, "asset"), []byte("after!"))
				batch := workspacePath(t, w, "asset")
				writeTest(t, filepath.Join(root, "second"), []byte("two"))
				workspacePath(t, w, "second") // retain the earlier batch too
				changed, err := query(WorkspaceSelection{})
				if err != nil {
					t.Fatal(err)
				}
				if changed.Files != 3 || len(changed.Changes) != 3 {
					t.Fatalf("lost workspace paths: %+v", changed)
				}
				if batch.Backend != "polling" && (changed.Observation.Mode != "targeted" || changed.Hashed != 9) {
					t.Fatalf("native change reread unrelated bytes: %+v", changed)
				}
				combined, err := query(WorkspaceSelection{Combined: true})
				if err != nil || combined.Branch != "main" || len(combined.Staged) == 0 || len(combined.Unstaged) == 0 {
					t.Fatalf("combined selection differs: %+v %v", combined, err)
				}
				if _, err = query(WorkspaceSelection{Revision: "missing"}); err == nil {
					t.Fatal("unknown revision accepted")
				}
				recovered, err := query(WorkspaceSelection{})
				if err != nil || !recovered.Observation.Full {
					t.Fatal("failed query did not recover fully", err)
				}
				if err = w.Invalidate(); err != nil {
					t.Fatal(err)
				}
				audited, err := query(WorkspaceSelection{})
				if err != nil || !audited.Observation.Full || audited.Hashed != 4*1024*1024+9 {
					t.Fatal("explicit audit reused working hashes", err)
				}
				// Cancellation before a wait must leave queued changes observable.
				writeTest(t, filepath.Join(root, "asset"), []byte("cancel"))
				ctx, cancel := context.WithCancel(context.Background())
				cancel()
				if _, err = w.Wait(ctx, time.Second); !errors.Is(err, context.Canceled) {
					t.Fatal(err)
				}
				workspacePath(t, w, "asset")
				cancelled, err := query(WorkspaceSelection{})
				if err != nil || cancelled.Files != 3 {
					t.Fatal("cancelled wait lost queued changes", err)
				}
				if err = w.Close(); err != nil {
					t.Fatal(err)
				}
				if err = w.Close(); err != nil {
					t.Fatal("close is not idempotent", err)
				}
				if _, err = w.Wait(context.Background(), 0); !errors.Is(err, ErrWorkspaceClosed) {
					t.Fatal(err)
				}
				if err = w.Invalidate(); !errors.Is(err, ErrWorkspaceClosed) {
					t.Fatal(err)
				}
			})
		}
	}
}
func TestWorkspacePollingWaitCancellationAndValidation(t *testing.T) {
	root := tempDirectory(t)
	storage := filepath.Join(root, ".syrnck")
	s := openTest(t, storage, nil)
	w := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: true})
	if _, err := w.Status(s, WorkspaceSelection{}); err != nil {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	start := time.Now()
	batch, err := w.Wait(context.Background(), 130*time.Millisecond)
	if err != nil || !batch.Polling || !batch.Changed() || time.Since(start) < 120*time.Millisecond {
		t.Fatal("polling returned after an internal 50 ms slice", batch, err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancel()
	start = time.Now()
	if _, err = w.Wait(ctx, time.Minute); !errors.Is(err, context.DeadlineExceeded) || time.Since(start) > time.Second {
		t.Fatal("idle wait cancellation did not finish promptly", err)
	}
	for _, duration := range []time.Duration{-1, time.Minute + 1} {
		if _, err := w.Wait(context.Background(), duration); err == nil {
			t.Fatal("invalid timeout accepted")
		}
	}
	if _, err := w.Status(nil, WorkspaceSelection{}); err == nil {
		t.Fatal("nil Store accepted")
	}
	if _, err := w.Status(s, WorkspaceSelection{}); !errors.Is(err, ErrClosed) {
		t.Fatal(err)
	}
	available, err := Open(storage, Options{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = available.Close() })
	data, err := w.Status(available, WorkspaceSelection{})
	if err != nil || !decodeWorkspace(t, data).Observation.Full {
		t.Fatal("closed Store query did not invalidate observation", err)
	}
	if err := available.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := w.Status(s, WorkspaceSelection{Revision: "id", Branch: "main"}); err == nil {
		t.Fatal("ambiguous selector accepted")
	}
	if _, err := w.Status(s, WorkspaceSelection{Branch: "bad\x00name"}); err == nil {
		t.Fatal("NUL selector accepted")
	}
	if _, err := OpenWorkspace(root+"\x00", storage, WorkspaceOptions{}); err == nil {
		t.Fatal("NUL path accepted")
	}
	if _, err := OpenWorkspace(root, storage, WorkspaceOptions{MaxPaths: 65537}); err == nil {
		t.Fatal("excessive path budget accepted")
	}
	if _, err := OpenWorkspace(root, storage, WorkspaceOptions{MaxDirectories: 65537}); err == nil {
		t.Fatal("excessive directory budget accepted")
	}
	if err := os.MkdirAll(filepath.Join(root, "nested"), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := OpenWorkspace(root, root, WorkspaceOptions{}); err == nil {
		t.Fatal("Store outside workspace accepted")
	}
}
func TestWorkspaceConcurrentQueriesAndClose(t *testing.T) {
	root := tempDirectory(t)
	storage := filepath.Join(root, ".syrnck")
	s := openTest(t, storage, nil)
	writeTest(t, filepath.Join(root, "asset"), []byte("one"))
	w := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: true})
	var group sync.WaitGroup
	for i := 0; i < 8; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			data, err := w.Status(s, WorkspaceSelection{})
			if err != nil || !json.Valid(data) {
				t.Errorf("concurrent status: %s %v", data, err)
			}
		}()
	}
	group.Wait()
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := w.Status(s, WorkspaceSelection{}); !errors.Is(err, ErrWorkspaceClosed) {
		t.Fatal(err)
	}
}
