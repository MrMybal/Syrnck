//go:build !windows

package main

import "path/filepath"

func canonicalExisting(path string) (string, error) { return filepath.EvalSymlinks(path) }
