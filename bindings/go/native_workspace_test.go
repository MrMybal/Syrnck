package syrnck

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
)

func TestNativeWorkspaceCapturePlanAndOwnedCheckout(t *testing.T) {
	for _, encrypted := range []bool{false, true} {
		t.Run(fmt.Sprint(encrypted), func(t *testing.T) {
			root := tempDirectory(t)
			storage := filepath.Join(root, ".syrnck")
			var key []byte
			if encrypted {
				key = bytes.Repeat([]byte{94}, 32)
			}
			store := openTest(t, storage, key)
			workspace := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: true})
			writeTest(t, filepath.Join(root, "asset"), []byte("first local bytes"))
			initial, err := workspace.NativeState(store)
			if err != nil {
				t.Fatal(err)
			}
			if initial.Version != 1 || initial.Target != "main" || initial.Revision != nil || initial.Reference != "heads/main" || initial.Staging {
				t.Fatalf("unexpected unborn state: %+v", initial)
			}
			id, err := workspace.Capture(store, initial.Expected(), "Native Go capture")
			if err != nil || len(id) != 64 {
				t.Fatal("capture failed", err)
			}
			current, err := workspace.NativeState(store)
			if err != nil || current.Revision == nil || current.Revision.ID != id {
				t.Fatal("capture state differs", err)
			}
			manifest, err := store.PrepareBytes([]byte("incoming complete bytes"), Chunking{})
			if err != nil {
				t.Fatal(err)
			}
			document, _ := json.Marshal(map[string]any{"version": 1, "parents": []string{id}, "message": "native Go candidate", "timestamp": 0,
				"files": map[string]any{"asset/leaf": map[string]any{"manifest": manifest, "executable": false}}})
			candidate, err := store.Put("revision", document)
			if err != nil {
				t.Fatal(err)
			}
			refs, err := store.Refs()
			if err != nil {
				t.Fatal(err)
			}
			for _, operation := range []func() error{
				func() error { _, e := workspace.Capture(store, initial.Expected(), "stale capture"); return e },
				func() error { _, e := workspace.PlanCheckout(store, initial.Expected(), candidate.ID); return e },
				func() error {
					_, e := workspace.CheckoutOwned(store, initial.Expected(), candidate.ID, strings.Repeat("a", 64))
					return e
				},
			} {
				if err := operation(); err == nil {
					t.Fatal("stale expectation accepted")
				}
			}
			after, _ := store.Refs()
			a, _ := json.Marshal(refs)
			b, _ := json.Marshal(after)
			if !bytes.Equal(a, b) {
				t.Fatal("stale native operation changed refs")
			}
			plan, err := workspace.PlanCheckout(store, current.Expected(), candidate.ID)
			if err != nil {
				t.Fatal(err)
			}
			var report struct {
				Ready bool `json:"ready"`
			}
			if err := json.Unmarshal(plan, &report); err != nil || !report.Ready {
				t.Fatalf("clean candidate blocked: %s %v", plan, err)
			}
			writeTest(t, filepath.Join(root, "asset"), []byte("uncommitted local work"))
			plan, err = workspace.PlanCheckout(store, current.Expected(), candidate.ID)
			if err != nil {
				t.Fatal(err)
			}
			_ = json.Unmarshal(plan, &report)
			if report.Ready {
				t.Fatal("dirty checkout plan ready")
			}
			if _, err := workspace.CheckoutOwned(store, current.Expected(), candidate.ID, strings.Repeat("a", 64)); err == nil {
				t.Fatal("dirty workspace overwritten")
			}
			writeTest(t, filepath.Join(root, "asset"), []byte("first local bytes"))
			completed, err := workspace.CheckoutOwned(store, current.Expected(), candidate.ID, strings.Repeat("a", 64))
			if err != nil {
				t.Fatal(err)
			}
			var transaction struct {
				ID string `json:"id"`
			}
			if err = json.Unmarshal(completed, &transaction); err != nil {
				t.Fatal(err)
			}
			receipt, err := workspace.CheckoutStatusContext(context.Background(), store, transaction.ID)
			var proof struct {
				Pending              bool `json:"pending"`
				State, Owner, Target string
			}
			if err != nil {
				t.Fatal(err)
			}
			if err = json.Unmarshal(receipt, &proof); err != nil || proof.Pending || proof.State != "committed" || proof.Owner != strings.Repeat("a", 64) || proof.Target != candidate.ID {
				t.Fatalf("incorrect completion proof: %s %v", receipt, err)
			}
			if _, err = workspace.CheckoutStatusContext(context.Background(), store, ""); err == nil {
				t.Fatal("empty transaction accepted")
			}
			if _, err = workspace.CheckoutStatusContext(context.Background(), store, strings.Repeat("e", 64)); err == nil {
				t.Fatal("unknown transaction accepted")
			}
			data, err := os.ReadFile(filepath.Join(root, "asset", "leaf"))
			if err != nil || !bytes.Equal(data, []byte("incoming complete bytes")) {
				t.Fatal("native transition lost bytes", err)
			}
			final, err := workspace.NativeState(store)
			if err != nil || final.Revision == nil || final.Revision.ID != candidate.ID {
				t.Fatal("native checkout state differs", err)
			}
			status, err := workspace.Status(store, WorkspaceSelection{})
			if err != nil || !decodeWorkspace(t, status).Observation.Full || !decodeWorkspace(t, status).Clean {
				t.Fatal("native mutations left stale observation", err)
			}
			if _, err := workspace.RecoverOwned(store, current.Target, candidate.ID, strings.Repeat("a", 64)); err == nil {
				t.Fatal("nonexistent journal recovered")
			}
			if _, err := workspace.RecoverOwnedPreserving(store, current.Target, candidate.ID, strings.Repeat("a", 64)); err == nil {
				t.Fatal("preserving recovery accepted absent journal")
			}
			ctx, cancel := context.WithCancel(context.Background())
			cancel()
			if _, err := workspace.CaptureContext(ctx, store, final.Expected(), "cancelled"); !errors.Is(err, context.Canceled) {
				t.Fatalf("cancelled capture: %v", err)
			}
			if _, err := workspace.NativeStateContext(ctx, store); !errors.Is(err, context.Canceled) {
				t.Fatalf("cancelled state: %v", err)
			}
			other := openTest(t, filepath.Join(tempDirectory(t), "store"), nil)
			if _, err := workspace.NativeState(other); err == nil {
				t.Fatal("workspace accepted another Store")
			}
			if _, err := workspace.Status(other, WorkspaceSelection{}); err == nil {
				t.Fatal("observation accepted another Store")
			}
			if _, err := workspace.Capture(store, final.Expected(), "bad\x00message"); err == nil {
				t.Fatal("NUL message accepted")
			}
			if _, err := workspace.NativeState(nil); err == nil {
				t.Fatal("nil Store accepted")
			}
			if err := workspace.Close(); err != nil {
				t.Fatal(err)
			}
			if _, err := workspace.NativeState(store); !errors.Is(err, ErrWorkspaceClosed) {
				t.Fatal("closed workspace accepted", err)
			}
		})
	}
}

func TestNativeConcurrentCaptureUsesOriginalExpectedRevision(t *testing.T) {
	root := tempDirectory(t)
	storage := filepath.Join(root, ".syrnck")
	store := openTest(t, storage, nil)
	workspace := openWorkspaceTest(t, root, storage, WorkspaceOptions{Polling: true})
	writeTest(t, filepath.Join(root, "asset"), []byte("complete concurrent snapshot"))
	initial, err := workspace.NativeState(store)
	if err != nil {
		t.Fatal(err)
	}
	var group sync.WaitGroup
	var winners atomic.Uint32
	for i := 0; i < 8; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			if _, err := workspace.Capture(store, initial.Expected(), "concurrent SDK snapshot"); err == nil {
				winners.Add(1)
			}
		}()
	}
	group.Wait()
	if winners.Load() != 1 {
		t.Fatalf("expected one guarded capture, got %d", winners.Load())
	}
	final, err := workspace.NativeState(store)
	if err != nil || final.Revision == nil {
		t.Fatal(err)
	}
	data, err := store.Get(*final.Revision)
	if err != nil {
		t.Fatal(err)
	}
	var document struct {
		Parents []string `json:"parents"`
	}
	if err := json.Unmarshal(data, &document); err != nil || len(document.Parents) != 0 {
		t.Fatal("stale capture was retargeted", err)
	}
}
