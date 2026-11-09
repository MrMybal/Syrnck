//go:build windows && (amd64 || arm64)

package syrnck

import (
	"errors"
	"runtime"
	"unsafe"
)

type nativeSecretKeyHandle uintptr

func keyOutput(data []byte, count uintptr, code int) ([]byte, error) {
	if err := status(code); err != nil {
		return nil, err
	}
	if count != ProtectedKeySize {
		return nil, errors.New("invalid native protected key length")
	}
	return data, nil
}
func nativeProtectKey(key, password []byte, costs *[2]uint32) ([]byte, error) {
	if err := loadLibrary(); err != nil {
		return nil, err
	}
	out := make([]byte, ProtectedKeySize)
	var count uintptr
	code, _, _ := library.functions["key_protect"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(key))), uintptr(len(key)), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(costs)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(key)
	runtime.KeepAlive(password)
	runtime.KeepAlive(costs)
	runtime.KeepAlive(out)
	return keyOutput(out, count, int(code))
}
func nativeChangeKeyPassword(envelope, before, after []byte, costs *[2]uint32) ([]byte, error) {
	if err := loadLibrary(); err != nil {
		return nil, err
	}
	out := make([]byte, ProtectedKeySize)
	var count uintptr
	code, _, _ := library.functions["key_change_password"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(envelope))), uintptr(len(envelope)), uintptr(unsafe.Pointer(unsafe.SliceData(before))), uintptr(len(before)), uintptr(unsafe.Pointer(unsafe.SliceData(after))), uintptr(len(after)), uintptr(unsafe.Pointer(costs)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(envelope)
	runtime.KeepAlive(before)
	runtime.KeepAlive(after)
	runtime.KeepAlive(costs)
	runtime.KeepAlive(out)
	return keyOutput(out, count, int(code))
}
func nativeOpenKey(envelope, password []byte) (nativeSecretKeyHandle, error) {
	if err := loadLibrary(); err != nil {
		return 0, err
	}
	var result nativeSecretKeyHandle
	code, _, _ := library.functions["secret_key_open"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(envelope))), uintptr(len(envelope)), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(envelope)
	runtime.KeepAlive(password)
	return result, status(int(code))
}
func nativeCloseKey(key nativeSecretKeyHandle) error {
	code, _, _ := library.functions["secret_key_close"].Call(uintptr(key))
	return status(int(code))
}
func nativeOpenWithSecret(path []byte, create bool, key nativeSecretKeyHandle) (nativeHandle, error) {
	var result nativeHandle
	var flag uintptr
	if create {
		flag = 1
	}
	code, _, _ := library.functions["store_open_secret"].Call(uintptr(unsafe.Pointer(&path[0])), flag, uintptr(key), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(path)
	return result, status(int(code))
}
