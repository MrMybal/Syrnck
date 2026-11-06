package syrnck

import (
	"context"
	"errors"
	"runtime"
	"sync"
)

var errNativeCancelled = errors.New("syrnck: native operation cancelled")
var zeroCancellationHandle nativeCancellationHandle

// contextMutex keeps the existing Lock/Unlock order while allowing workspace
// calls to abandon serialization waits. Its zero value is ready to use.
type contextMutex struct {
	once sync.Once
	held chan struct{}
}

func (m *contextMutex) LockContext(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	m.once.Do(func() { m.held = make(chan struct{}, 1) })
	select {
	case m.held <- struct{}{}:
		if err := ctx.Err(); err != nil {
			m.Unlock()
			return err
		}
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}
func (m *contextMutex) Lock()   { _ = m.LockContext(context.Background()) }
func (m *contextMutex) Unlock() { <-m.held }

// The caller pins its OS thread for native diagnostics. The AfterFunc callback
// owns only the native cancellation source, never a Store/workspace handle.
// Stop/join the callback before destroying that source, even on errors/panics.
func withNativeCancellation(ctx context.Context, call func(nativeCancellationHandle) error) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if ctx.Done() == nil {
		return call(zeroCancellationHandle)
	}
	cancellation, err := nativeCancellationCreate()
	if err != nil {
		return err
	}
	finished := make(chan struct{})
	stop := context.AfterFunc(ctx, func() {
		runtime.LockOSThread()
		_ = nativeCancellationRequest(cancellation)
		runtime.UnlockOSThread()
		close(finished)
	})
	defer func() {
		if !stop() {
			<-finished
		}
		_ = nativeCancellationClose(cancellation)
	}()
	if err := ctx.Err(); err != nil {
		return err
	}
	err = call(cancellation)
	if errors.Is(err, errNativeCancelled) {
		if cause := ctx.Err(); cause != nil {
			return cause
		}
		return context.Canceled
	}
	// A completed native success/error wins over a later cancellation request.
	return err
}
