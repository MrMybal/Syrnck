//go:build windows && (amd64 || arm64)

package syrnck

import (
	"runtime"
	"unsafe"
)

type nativeSigningKeyHandle uintptr

func nativeProtectSigningKey(key, password []byte, costs *[2]uint32) ([]byte, error) {
	if err := loadLibrary(); err != nil {
		return nil, err
	}
	out := make([]byte, ProtectedKeySize)
	var count uintptr
	code, _, _ := library.functions["signing_key_protect"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(key))), uintptr(len(key)), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(costs)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(key)
	runtime.KeepAlive(password)
	runtime.KeepAlive(costs)
	runtime.KeepAlive(out)
	return keyOutput(out, count, int(code))
}
func nativeChangeSigningKeyPassword(envelope, before, after []byte, costs *[2]uint32) ([]byte, error) {
	if err := loadLibrary(); err != nil {
		return nil, err
	}
	out := make([]byte, ProtectedKeySize)
	var count uintptr
	code, _, _ := library.functions["signing_key_change_password"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(envelope))), uintptr(len(envelope)), uintptr(unsafe.Pointer(unsafe.SliceData(before))), uintptr(len(before)), uintptr(unsafe.Pointer(unsafe.SliceData(after))), uintptr(len(after)), uintptr(unsafe.Pointer(costs)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(envelope)
	runtime.KeepAlive(before)
	runtime.KeepAlive(after)
	runtime.KeepAlive(costs)
	runtime.KeepAlive(out)
	return keyOutput(out, count, int(code))
}
func nativeOpenSigner(envelope, password []byte) (nativeSigningKeyHandle, error) {
	if err := loadLibrary(); err != nil {
		return 0, err
	}
	var result nativeSigningKeyHandle
	code, _, _ := library.functions["signing_key_open"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(envelope))), uintptr(len(envelope)), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(envelope)
	runtime.KeepAlive(password)
	return result, status(int(code))
}
func nativeCloseSigner(key nativeSigningKeyHandle) error {
	code, _, _ := library.functions["signing_key_close"].Call(uintptr(key))
	return status(int(code))
}
func nativeSignerMetadata(key nativeSigningKeyHandle) ([]byte, error) {
	var ptr, length uintptr
	code, _, _ := library.functions["signing_key_public_json"].Call(uintptr(key), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	return workspaceJSONResult(int(code), ptr, length)
}
func nativeUseSigner(h nativeHandle, name, email []byte, offset int32, key nativeSigningKeyHandle, required bool) error {
	var flag uintptr
	if required {
		flag = 1
	}
	code, _, _ := library.functions["store_use_native_signer"].Call(uintptr(h), uintptr(unsafe.Pointer(&name[0])), uintptr(unsafe.Pointer(&email[0])), uintptr(uint32(offset)), uintptr(key), flag)
	runtime.KeepAlive(name)
	runtime.KeepAlive(email)
	return status(int(code))
}
