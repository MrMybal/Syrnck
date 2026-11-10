//go:build windows

package main

import (
	"errors"
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"
)

func hideProcess(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
}

func testDirectory(t testing.TB) string {
	t.Helper()
	directory := t.TempDir()
	t.Cleanup(func() {
		deadline := time.Now().Add(2 * time.Second)
		for {
			err := os.RemoveAll(directory)
			if err == nil || !errors.Is(err, syscall.ERROR_DIR_NOT_EMPTY) {
				return
			}
			// No engine action/assertion is retried. Windows may briefly retain
			// deleted directory entries after all native owners have closed.
			if !time.Now().Before(deadline) {
				t.Errorf("temporary directory cleanup: %v", err)
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
	})
	return directory
}
