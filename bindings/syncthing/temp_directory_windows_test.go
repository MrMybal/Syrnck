package syncthing

import (
	"errors"
	"os"
	"syscall"
	"testing"
	"time"
)

func tempDirectory(t testing.TB) string {
	t.Helper()
	dir := t.TempDir()
	// Register before openStore's Close cleanup, so handles close first. Keep
	// testing.TempDir's own cleanup as a final safeguard for this owned path.
	t.Cleanup(func() {
		deadline := time.Now().Add(2 * time.Second)
		waited := false
		for {
			err := os.RemoveAll(dir)
			if err == nil {
				return
			}
			// Leave other errors to testing.TempDir's standard cleanup and
			// Windows retry policy. No native operation/assertion is retried.
			if !errors.Is(err, syscall.ERROR_DIR_NOT_EMPTY) {
				return
			}
			// Windows can briefly retain a deleted directory entry. A
			// persistent failure must still fail the test at the deadline.
			if !time.Now().Before(deadline) {
				t.Errorf("temporary directory cleanup: %v", err)
				return
			}
			if !waited {
				t.Log("waiting for Windows temporary directory deletion")
				waited = true
			}
			time.Sleep(10 * time.Millisecond)
		}
	})
	return dir
}
