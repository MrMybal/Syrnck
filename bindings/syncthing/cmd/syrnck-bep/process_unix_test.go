//go:build !windows

package main

import (
	"os/exec"
	"testing"
)

func hideProcess(*exec.Cmd) {}

func testDirectory(t testing.TB) string { t.Helper(); return t.TempDir() }
