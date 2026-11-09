//go:build windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

var finalPath = syscall.NewLazyDLL("kernel32.dll").NewProc("GetFinalPathNameByHandleW")

// Resolve the opened object, including junctions and short-name aliases. A
// component walk unnecessarily requires access to every ancestor directory;
// an application can legitimately access its exact registered directory only.
func canonicalExisting(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	buffer := make([]uint16, 32768)
	length, _, callError := finalPath.Call(file.Fd(), uintptr(unsafe.Pointer(&buffer[0])), uintptr(len(buffer)), 0)
	if length == 0 {
		return "", callError
	}
	if length >= uintptr(len(buffer)) {
		return "", errors.New("resolved filesystem path exceeds limit")
	}
	resolved := syscall.UTF16ToString(buffer[:length])
	if strings.HasPrefix(resolved, `\\?\UNC\`) {
		resolved = `\\` + resolved[8:]
	} else if strings.HasPrefix(resolved, `\\?\`) {
		resolved = resolved[4:]
	}
	if !filepath.IsAbs(resolved) {
		return "", errors.New("unsupported resolved filesystem path")
	}
	return filepath.Clean(resolved), nil
}
