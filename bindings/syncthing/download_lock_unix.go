//go:build darwin || dragonfly || freebsd || linux || netbsd || openbsd

package syncthing

import (
	"errors"
	"os"
	"syscall"
)

func lockDownload(file *os.File) error {
	err := syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
	if errors.Is(err, syscall.EWOULDBLOCK) || errors.Is(err, syscall.EAGAIN) {
		return ErrFolderDownloadBusy
	}
	return err
}
