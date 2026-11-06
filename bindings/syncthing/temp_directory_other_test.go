//go:build !windows

package syncthing

import "testing"

func tempDirectory(t testing.TB) string {
	t.Helper()
	return t.TempDir()
}
