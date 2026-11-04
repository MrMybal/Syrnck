//go:build !windows

package syrnck

import "testing"

func tempDirectory(t *testing.T) string {
	t.Helper()
	return t.TempDir()
}
