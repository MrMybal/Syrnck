//go:build windows && (amd64 || arm64)

package syrnck

import (
	"runtime"
	"unsafe"
)

func nativeUseIdentity(h nativeHandle, name, email []byte, offset int32, seed []byte, required bool) error {
	var flag uintptr
	if required {
		flag = 1
	}
	r, _, _ := library.functions["store_use_native_identity"].Call(uintptr(h), uintptr(unsafe.Pointer(&name[0])), uintptr(unsafe.Pointer(&email[0])), uintptr(uint32(offset)), uintptr(unsafe.Pointer(unsafe.SliceData(seed))), uintptr(len(seed)), flag)
	runtime.KeepAlive(name)
	runtime.KeepAlive(email)
	runtime.KeepAlive(seed)
	return status(int(r))
}
func nativeClearIdentity(h nativeHandle) error {
	r, _, _ := library.functions["store_clear_native_identity"].Call(uintptr(h))
	return status(int(r))
}
func nativeIdentityMetadata(h nativeHandle) ([]byte, error) {
	var ptr, length uintptr
	r, _, _ := library.functions["store_native_identity_json"].Call(uintptr(h), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	return workspaceJSONResult(int(r), ptr, length)
}
func nativePrepareRevision(h nativeHandle, data []byte, cancellation nativeCancellationHandle) (string, error) {
	var id [65]byte
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	r, _, _ := library.functions["prepare_native_revision_ex"].Call(uintptr(h), uintptr(unsafe.Pointer(unsafe.SliceData(data))), uintptr(len(data)), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&id[0])))
	runtime.KeepAlive(data)
	runtime.KeepAlive(operation)
	if err := status(int(r)); err != nil {
		return "", err
	}
	return string(id[:64]), nil
}
