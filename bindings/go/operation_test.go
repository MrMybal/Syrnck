package syrnck

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

func TestWorkspaceContextSerializationWaits(t *testing.T) {
	root := tempDirectory(t)
	storage := filepath.Join(root, ".syrnck")
	store := openTest(t, storage, nil)
	w := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: true})
	if _, err := w.Status(store, WorkspaceSelection{}); err != nil {
		t.Fatal(err)
	}
	for _, lock := range []*contextMutex{&w.mu, &store.mu} {
		func() {
			lock.Lock()
			defer lock.Unlock()
			ctx, cancel := context.WithTimeout(context.Background(), 25*time.Millisecond)
			defer cancel()
			finished := make(chan error, 1)
			go func() { _, err := w.StatusContext(ctx, store, WorkspaceSelection{}); finished <- err }()
			select {
			case err := <-finished:
				if !errors.Is(err, context.DeadlineExceeded) {
					t.Fatal("serialization deadline lost", err)
				}
			case <-time.After(time.Second):
				t.Fatal("context could not abandon a held workspace/Store lock")
			}
		}()
	}
	data, err := w.Status(store, WorkspaceSelection{})
	if err != nil || !decodeWorkspace(t, data).Observation.Full {
		t.Fatal("failed query did not require full recovery", err)
	}
	func() {
		w.mu.Lock()
		defer w.mu.Unlock()
		ctx, cancel := context.WithTimeout(context.Background(), 25*time.Millisecond)
		defer cancel()
		finished := make(chan error, 1)
		go func() { _, err := w.Wait(ctx, time.Second); finished <- err }()
		select {
		case err := <-finished:
			if !errors.Is(err, context.DeadlineExceeded) {
				t.Fatal(err)
			}
		case <-time.After(time.Second):
			t.Fatal("wait could not abandon a held workspace lock")
		}
	}()
}

func TestWorkspaceContextStopsActiveNativeScan(t *testing.T) {
	root := tempDirectory(t)
	storage := filepath.Join(root, ".syrnck")
	store := openTest(t, storage, nil)
	file, err := os.Create(filepath.Join(root, "large"))
	if err != nil {
		t.Fatal(err)
	}
	const size = 128 * 1024 * 1024
	err = file.Truncate(size)
	closeErr := file.Close()
	if err != nil || closeErr != nil {
		t.Fatal(err, closeErr)
	}
	w := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: true})
	if _, err := w.Status(store, WorkspaceSelection{}); err != nil {
		t.Fatal(err)
	}
	pointer := filepath.Join(storage, "working-index.json")
	if _, err := os.Stat(pointer); err != nil {
		t.Fatal(err)
	}
	if err := w.Invalidate(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	entered := make(chan error, 1)
	// The full native scan removes its old cache pointer before reading bytes.
	// Cancel at that observed native boundary rather than after a guessed sleep.
	go func() {
		deadline := time.Now().Add(3 * time.Second)
		for time.Now().Before(deadline) {
			_, err := os.Stat(pointer)
			if errors.Is(err, os.ErrNotExist) {
				cancel()
				entered <- nil
				return
			}
			if err != nil {
				cancel()
				entered <- err
				return
			}
			time.Sleep(time.Millisecond)
		}
		cancel()
		entered <- errors.New("full native scan boundary was not observed")
	}()
	data, err := w.StatusContext(ctx, store, WorkspaceSelection{})
	if boundary := <-entered; boundary != nil {
		t.Fatal(boundary)
	}
	if !errors.Is(err, context.Canceled) || len(data) != 0 {
		t.Fatalf("active native scan returned stale/partial output: %s %v", data, err)
	}
	recovered, err := w.Status(store, WorkspaceSelection{})
	if err != nil {
		t.Fatal(err)
	}
	report := decodeWorkspace(t, recovered)
	if !report.Observation.Full || report.Hashed != size || report.Files != 1 {
		t.Fatalf("cancelled native inventory was reused: %+v", report)
	}
	// A completed query can release the ordinary Store lock immediately.
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	reopened, err := Open(storage, Options{})
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	if _, err := w.StatusContext(context.Background(), reopened, WorkspaceSelection{}); err != nil {
		t.Fatal(err)
	}
}

func TestNativeCancellationLifetimeAndCompletion(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	for i := 0; i < 64; i++ {
		ctx, cancel := context.WithCancel(context.Background())
		err := withNativeCancellation(ctx, func(source nativeCancellationHandle) error {
			cancel()
			if err := nativeCancellationRequest(source); err != nil {
				return err
			}
			return errNativeCancelled
		})
		cancel()
		if !errors.Is(err, context.Canceled) {
			t.Fatal("native cancellation not mapped to its context", err)
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	err := withNativeCancellation(ctx, func(source nativeCancellationHandle) error {
		cancel()
		// A native call that already completed successfully retains its result.
		return nativeCancellationRequest(source)
	})
	if err != nil {
		t.Fatal("late cancellation replaced a completed native success", err)
	}
	ctx, cancel = context.WithCancel(context.Background())
	want := errors.New("completed native failure")
	err = withNativeCancellation(ctx, func(source nativeCancellationHandle) error {
		cancel()
		return want
	})
	if !errors.Is(err, want) {
		t.Fatal("late cancellation hid a native failure", err)
	}
}

func TestWorkspacePrecancelledOpenAndStatus(t *testing.T) {
	root := tempDirectory(t)
	storage := filepath.Join(root, ".syrnck")
	store := openTest(t, storage, nil)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if w, err := OpenWorkspaceContext(ctx, root, storage, WorkspaceOptions{}); w != nil || !errors.Is(err, context.Canceled) {
		t.Fatal("cancelled open created a session", w, err)
	}
	w := openWorkspaceTest(t, root, storage, WorkspaceOptions{})
	if data, err := w.StatusContext(ctx, store, WorkspaceSelection{}); data != nil || !errors.Is(err, context.Canceled) {
		t.Fatal("cancelled status entered a scan", data, err)
	}
	expired, stop := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer stop()
	if _, err := OpenWorkspaceContext(expired, root, storage, WorkspaceOptions{}); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatal(err)
	}
	if data, err := w.StatusContext(context.Background(), store, WorkspaceSelection{}); err != nil || !decodeWorkspace(t, data).Observation.Full {
		t.Fatal("pre-cancelled query changed initial inventory", fmt.Sprint(err))
	}
}
