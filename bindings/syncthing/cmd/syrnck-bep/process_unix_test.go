//go:build !windows

package main

import "os/exec"

func hideProcess(*exec.Cmd) {}
