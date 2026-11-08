package syrnck

import (
	"context"
	"encoding/json"
	"errors"
	"runtime"
)

// NativeExpectation binds the exact target, revision and materialization object.
// An empty Revision requires an unborn target; empty Materialization requires
// no policy reference. Do not replace it with a newer value on failure:
// reload state, preserve the prior result and replan the operation.
type NativeExpectation struct{ Target, Revision, Materialization string }

type NativeMaterialization struct {
	Version  uint32   `json:"version"`
	Complete bool     `json:"complete"`
	Prefixes []string `json:"prefixes"`
	Object   *string  `json:"object"`
}

type NativeState struct {
	Version         uint32                `json:"version"`
	Workspace       string                `json:"workspace"`
	Target          string                `json:"target"`
	Reference       string                `json:"reference"`
	Revision        *ObjectKey            `json:"revision"`
	Head            json.RawMessage       `json:"head"`
	Materialization NativeMaterialization `json:"materialization"`
	Checkout        json.RawMessage       `json:"checkout"`
	Staging         bool                  `json:"staging"`
}

func (s NativeState) Expected() NativeExpectation {
	expected := NativeExpectation{Target: s.Target}
	if s.Revision != nil {
		expected.Revision = s.Revision.ID
	}
	if s.Materialization.Object != nil {
		expected.Materialization = *s.Materialization.Object
	}
	return expected
}

func (w *Workspace) nativeCall(ctx context.Context, store *Store, mode uint32, expected NativeExpectation, candidate, owner, message string) (json.RawMessage, error) {
	if store == nil {
		return nil, errors.New("syrnck: nil store")
	}
	inputs := []string{expected.Target, expected.Revision, candidate, owner, message, expected.Materialization}
	var arguments [6][]byte
	for i, value := range inputs {
		if value == "" && i != 4 {
			continue
		}
		data, err := text(value)
		if err != nil {
			return nil, err
		}
		arguments[i] = data
	}
	if mode != 0 && expected.Target == "" {
		return nil, errors.New("syrnck: native target expectation required")
	}
	if len(message) > 65536 {
		return nil, errors.New("syrnck: capture message exceeds 64 KiB")
	}
	if err := w.mu.LockContext(ctx); err != nil {
		return nil, err
	}
	defer w.mu.Unlock()
	if w.handle == zeroWorkspaceHandle {
		return nil, ErrWorkspaceClosed
	}
	var result []byte
	err := store.withContext(ctx, func() error {
		return withNativeCancellation(ctx, func(cancellation nativeCancellationHandle) error {
			var failure error
			result, failure = nativeWorkspaceNative(w.handle, store.handle, mode, arguments, cancellation)
			return failure
		})
	})
	if err != nil {
		// Match native continuity invalidation even when a closed Store or a
		// context stops the call before it reaches the ABI.
		runtime.LockOSThread()
		_ = nativeWorkspaceInvalidate(w.handle)
		runtime.UnlockOSThread()
	}
	return result, err
}

func (w *Workspace) NativeState(store *Store) (NativeState, error) {
	return w.NativeStateContext(context.Background(), store)
}

// NativeStateContext reads the exact bound Store/context/directory state without
// recovering a journal or changing refs. Close Store before idle/network waits.
func (w *Workspace) NativeStateContext(ctx context.Context, store *Store) (state NativeState, err error) {
	data, err := w.nativeCall(ctx, store, 0, NativeExpectation{}, "", "", "")
	if err == nil {
		err = json.Unmarshal(data, &state)
	}
	return
}

func (w *Workspace) Capture(store *Store, expected NativeExpectation, message string) (string, error) {
	return w.CaptureContext(context.Background(), store, expected, message)
}

// CaptureContext retains a native snapshot using ordinary ignore, materialization,
// authored-commit and detached-HEAD rules. It refuses stale target/revision state
// and existing checkout/staging work. Any error may follow publication: reload.
func (w *Workspace) CaptureContext(ctx context.Context, store *Store, expected NativeExpectation, message string) (string, error) {
	data, err := w.nativeCall(ctx, store, 1, expected, "", "", message)
	if err != nil {
		return "", err
	}
	var result struct {
		Revision string `json:"revision"`
	}
	if err := json.Unmarshal(data, &result); err != nil {
		return "", err
	}
	return result.Revision, nil
}

func (w *Workspace) PlanCheckout(store *Store, expected NativeExpectation, candidate string) (json.RawMessage, error) {
	return w.PlanCheckoutContext(context.Background(), store, expected, candidate)
}

// PlanCheckoutContext verifies the candidate closure and reports ready/conflicts.
// It changes neither refs nor working files. CheckoutOwned rechecks the plan.
func (w *Workspace) PlanCheckoutContext(ctx context.Context, store *Store, expected NativeExpectation, candidate string) (json.RawMessage, error) {
	return w.nativeCall(ctx, store, 2, expected, candidate, "", "")
}

func (w *Workspace) CheckoutOwned(store *Store, expected NativeExpectation, candidate, owner string) (json.RawMessage, error) {
	return w.CheckoutOwnedContext(context.Background(), store, expected, candidate, owner)
}

// CheckoutOwnedContext applies to a clean selected native workspace through its
// recoverable journal. Owner is an exact 64-hex durable service identity. Dirty
// files, stale expectations and foreign journals are refused, never overwritten.
// On error reload native state before retry/recovery; output is not proof of the
// publication outcome. Success wins late cancellation and invalidates observation.
func (w *Workspace) CheckoutOwnedContext(ctx context.Context, store *Store, expected NativeExpectation, candidate, owner string) (json.RawMessage, error) {
	return w.nativeCall(ctx, store, 3, expected, candidate, owner, "")
}

func (w *Workspace) RecoverOwned(store *Store, target, candidate, owner string) (json.RawMessage, error) {
	return w.RecoverOwnedContext(context.Background(), store, target, candidate, owner)
}

// RecoverOwnedContext resumes only a pending journal with exactly this selected
// target, candidate and owner in this bound Store/workspace/context. Establish
// those values from the service's durable state. No pending journal is an error.
func (w *Workspace) RecoverOwnedContext(ctx context.Context, store *Store, target, candidate, owner string) (json.RawMessage, error) {
	return w.nativeCall(ctx, store, 4, NativeExpectation{Target: target}, candidate, owner, "")
}
