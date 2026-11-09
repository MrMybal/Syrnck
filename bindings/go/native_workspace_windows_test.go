//go:build windows && (amd64 || arm64)

package syrnck

import (
	"fmt"
	"path/filepath"
	"runtime"
	"sync"
	"testing"
)

// Vary the available goroutine stack at the real DLL boundary. The caller must
// receive both output slots even when a syscall or allocation grows the stack.
func nativeStateOnStack(w *Workspace, store *Store, depth int) (NativeState, error) {
	var padding [128]byte
	var state NativeState
	var err error
	if depth == 0 {
		state, err = w.NativeState(store)
	} else {
		state, err = nativeStateOnStack(w, store, depth-1)
	}
	runtime.KeepAlive(&padding)
	return state, err
}

func TestNativeWindowsWorkspaceOutputAcrossStackGrowth(t *testing.T) {
	root := tempDirectory(t)
	storage := filepath.Join(root, ".syrnck")
	store := openTest(t, storage, nil)
	workspace := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: true})
	writeTest(t, filepath.Join(root, "asset"), []byte("complete native output"))
	initial, err := workspace.NativeState(store)
	if err != nil {
		t.Fatal(err)
	}
	id, err := workspace.Capture(store, initial.Expected(), "Stable native output")
	if err != nil {
		t.Fatal(err)
	}
	want := initial.Expected()
	want.Revision = id
	for round := 0; round < 12; round++ {
		runtime.GC()
		failures := make(chan error, 16)
		var group sync.WaitGroup
		for worker := 0; worker < 16; worker++ {
			group.Add(1)
			go func(depth int) {
				defer group.Done()
				state, err := nativeStateOnStack(workspace, store, depth)
				if err != nil {
					failures <- err
				} else if state.Expected() != want {
					failures <- fmt.Errorf("native output changed: got %+v, want %+v", state.Expected(), want)
				}
			}(round + worker)
		}
		group.Wait()
		close(failures)
		for failure := range failures {
			t.Fatal(failure)
		}
	}
}
