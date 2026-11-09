//go:build !windows && !darwin && !dragonfly && !freebsd && !linux && !netbsd && !openbsd

package syncthing

import (
	"errors"
	"os"
)

func lockDownload(*os.File) error {
	return errors.New("persistent download locking is unavailable on this platform")
}
