//go:build windows && (amd64 || arm64)

package syrnck

import (
	"runtime"
	"unsafe"
)

type nativeSealedKeyHandle uintptr

func nativeProtectSealedKey(key, password []byte, costs *[2]uint32) ([]byte, error) {
	if err := loadLibrary(); err != nil {
		return nil, err
	}
	out := make([]byte, ProtectedKeySize)
	var count uintptr
	code, _, _ := library.functions["sealed_key_protect"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(key))), uintptr(len(key)), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(costs)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(key)
	runtime.KeepAlive(password)
	runtime.KeepAlive(costs)
	runtime.KeepAlive(out)
	return keyOutput(out, count, int(code))
}
func nativeChangeSealedKeyPassword(envelope, before, after []byte, costs *[2]uint32) ([]byte, error) {
	if err := loadLibrary(); err != nil {
		return nil, err
	}
	out := make([]byte, ProtectedKeySize)
	var count uintptr
	code, _, _ := library.functions["sealed_key_change_password"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(envelope))), uintptr(len(envelope)), uintptr(unsafe.Pointer(unsafe.SliceData(before))), uintptr(len(before)), uintptr(unsafe.Pointer(unsafe.SliceData(after))), uintptr(len(after)), uintptr(unsafe.Pointer(costs)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(envelope)
	runtime.KeepAlive(before)
	runtime.KeepAlive(after)
	runtime.KeepAlive(costs)
	runtime.KeepAlive(out)
	return keyOutput(out, count, int(code))
}
func nativeOpenSealedKey(envelope, password []byte) (nativeSealedKeyHandle, error) {
	if err := loadLibrary(); err != nil {
		return 0, err
	}
	var result nativeSealedKeyHandle
	code, _, _ := library.functions["sealed_key_open"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(envelope))), uintptr(len(envelope)), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(envelope)
	runtime.KeepAlive(password)
	return result, status(int(code))
}
func nativeCloseSealedKey(key nativeSealedKeyHandle) error {
	code, _, _ := library.functions["sealed_key_close"].Call(uintptr(key))
	return status(int(code))
}
func nativeSealedKeyMetadata(key nativeSealedKeyHandle) ([]byte, error) {
	var ptr, length uintptr
	code, _, _ := library.functions["sealed_key_public_json"].Call(uintptr(key), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	return workspaceJSONResult(int(code), ptr, length)
}
