//go:build (linux || darwin) && cgo

package syrnck

/*
#include <syrnck.h>
*/
import "C"
import "unsafe"

func nativeSealSnapshot(source, wire nativeHandle, key nativeSealedKeyHandle, channel, roots, previous []byte,
	limits *[5]uint64, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr *C.char
	var length C.size_t
	operation := C.SyrnckOperation{cancellation: cancellation}
	code := C.syrnck_sealed_snapshot_json_ex(source, wire, key, ctext(channel), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(roots))),
		C.size_t(len(roots)), ctext(previous), (*C.SyrnckSealedLimits)(unsafe.Pointer(limits)), &operation, &ptr, &length)
	return workspaceJSONResult(int(code), ptr, length)
}
func nativeInspectSealed(wire nativeHandle, key nativeSealedKeyHandle, snapshot, channel []byte,
	limits *[5]uint64, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr *C.char
	var length C.size_t
	operation := C.SyrnckOperation{cancellation: cancellation}
	code := C.syrnck_sealed_inspect_json_ex(wire, key, ctext(snapshot), ctext(channel),
		(*C.SyrnckSealedLimits)(unsafe.Pointer(limits)), &operation, &ptr, &length)
	return workspaceJSONResult(int(code), ptr, length)
}
func nativeOpenSealed(wire, destination nativeHandle, key nativeSealedKeyHandle, snapshot, channel, checkpoint []byte,
	limits *[5]uint64, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr *C.char
	var length C.size_t
	operation := C.SyrnckOperation{cancellation: cancellation}
	code := C.syrnck_sealed_open_json_ex(wire, destination, key, ctext(snapshot), ctext(channel), ctext(checkpoint),
		(*C.SyrnckSealedLimits)(unsafe.Pointer(limits)), &operation, &ptr, &length)
	return workspaceJSONResult(int(code), ptr, length)
}
