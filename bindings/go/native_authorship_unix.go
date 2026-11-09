//go:build (linux || darwin) && cgo

package syrnck

/*
#include <syrnck.h>
*/
import "C"
import "unsafe"

func nativeUseIdentity(h nativeHandle, name, email []byte, offset int32, seed []byte, required bool) error {
	return status(int(C.syrnck_store_use_native_identity(h, ctext(name), ctext(email), C.int32_t(offset), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(seed))), C.size_t(len(seed)), C.bool(required))))
}
func nativeClearIdentity(h nativeHandle) error {
	return status(int(C.syrnck_store_clear_native_identity(h)))
}
func nativeIdentityMetadata(h nativeHandle) ([]byte, error) {
	var ptr *C.char
	var length C.size_t
	code := C.syrnck_store_native_identity_json(h, &ptr, &length)
	return workspaceJSONResult(int(code), ptr, length)
}
func nativePrepareRevision(h nativeHandle, data []byte, cancellation nativeCancellationHandle) (string, error) {
	operation := C.SyrnckOperation{cancellation: cancellation}
	var id [65]C.char
	code := C.syrnck_prepare_native_revision_ex(h, (*C.uchar)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)), &operation, &id[0])
	if err := status(int(code)); err != nil {
		return "", err
	}
	return C.GoStringN(&id[0], 64), nil
}
