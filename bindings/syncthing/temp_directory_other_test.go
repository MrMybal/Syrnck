//go:build !windows

package syncthing

import (
	"os/exec"
	"testing"
)

func hideReferenceProcess(*exec.Cmd) {}

func tempDirectory(t testing.TB) string {
	t.Helper()
	return t.TempDir()
}
