//go:build (linux || darwin) && cgo

package syrnck

/*
#include <syrnck.h>
*/
import "C"
import (
	"errors"
	"unsafe"
)

type nativeSecretKeyHandle = *C.SyrnckSecretKey

func nativeProtectKey(key, password []byte, costs *[2]uint32) ([]byte, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	out := make([]byte, ProtectedKeySize)
	var count C.size_t
	code := C.syrnck_key_protect((*C.uchar)(unsafe.Pointer(unsafe.SliceData(key))), C.size_t(len(key)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(password))), C.size_t(len(password)), (*C.SyrnckPasswordKdf)(unsafe.Pointer(costs)), (*C.uchar)(unsafe.Pointer(&out[0])), C.size_t(len(out)), &count)
	if err := status(int(code)); err != nil {
		return nil, err
	}
	if count != ProtectedKeySize {
		return nil, errors.New("invalid native protected key length")
	}
	return out, nil
}
func nativeChangeKeyPassword(envelope, before, after []byte, costs *[2]uint32) ([]byte, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	out := make([]byte, ProtectedKeySize)
	var count C.size_t
	code := C.syrnck_key_change_password((*C.uchar)(unsafe.Pointer(unsafe.SliceData(envelope))), C.size_t(len(envelope)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(before))), C.size_t(len(before)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(after))), C.size_t(len(after)), (*C.SyrnckPasswordKdf)(unsafe.Pointer(costs)), (*C.uchar)(unsafe.Pointer(&out[0])), C.size_t(len(out)), &count)
	if err := status(int(code)); err != nil {
		return nil, err
	}
	if count != ProtectedKeySize {
		return nil, errors.New("invalid native protected key length")
	}
	return out, nil
}
func nativeOpenKey(envelope, password []byte) (nativeSecretKeyHandle, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	var out nativeSecretKeyHandle
	code := C.syrnck_secret_key_open((*C.uchar)(unsafe.Pointer(unsafe.SliceData(envelope))), C.size_t(len(envelope)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(password))), C.size_t(len(password)), &out)
	return out, status(int(code))
}
func nativeCloseKey(key nativeSecretKeyHandle) error {
	return status(int(C.syrnck_secret_key_close(key)))
}
func nativeOpenWithSecret(path []byte, create bool, key nativeSecretKeyHandle) (nativeHandle, error) {
	var out nativeHandle
	code := C.syrnck_store_open_secret(ctext(path), C.bool(create), key, &out)
	return out, status(int(code))
}
