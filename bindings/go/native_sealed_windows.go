//go:build windows && (amd64 || arm64)

package syrnck

import (
	"runtime"
	"unsafe"
)

func nativeSealSnapshot(source, wire nativeHandle, key nativeSealedKeyHandle, channel, roots, previous []byte,
	limits *[5]uint64, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr, length uintptr
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	code, _, _ := library.functions["sealed_snapshot_json_ex"].Call(uintptr(source), uintptr(wire), uintptr(key),
		uintptr(unsafe.Pointer(&channel[0])), uintptr(unsafe.Pointer(unsafe.SliceData(roots))), uintptr(len(roots)),
		uintptr(unsafe.Pointer(&previous[0])), uintptr(unsafe.Pointer(limits)), uintptr(unsafe.Pointer(&operation)),
		uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	runtime.KeepAlive(channel)
	runtime.KeepAlive(roots)
	runtime.KeepAlive(previous)
	runtime.KeepAlive(limits)
	runtime.KeepAlive(operation)
	return workspaceJSONResult(int(code), ptr, length)
}
func nativeInspectSealed(wire nativeHandle, key nativeSealedKeyHandle, snapshot, channel []byte,
	limits *[5]uint64, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr, length uintptr
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	code, _, _ := library.functions["sealed_inspect_json_ex"].Call(uintptr(wire), uintptr(key),
		uintptr(unsafe.Pointer(&snapshot[0])), uintptr(unsafe.Pointer(&channel[0])), uintptr(unsafe.Pointer(limits)),
		uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	runtime.KeepAlive(snapshot)
	runtime.KeepAlive(channel)
	runtime.KeepAlive(limits)
	runtime.KeepAlive(operation)
	return workspaceJSONResult(int(code), ptr, length)
}
func nativeOpenSealed(wire, destination nativeHandle, key nativeSealedKeyHandle, snapshot, channel, checkpoint []byte,
	limits *[5]uint64, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr, length uintptr
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	code, _, _ := library.functions["sealed_open_json_ex"].Call(uintptr(wire), uintptr(destination), uintptr(key),
		uintptr(unsafe.Pointer(&snapshot[0])), uintptr(unsafe.Pointer(&channel[0])), uintptr(unsafe.Pointer(&checkpoint[0])),
		uintptr(unsafe.Pointer(limits)), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	runtime.KeepAlive(snapshot)
	runtime.KeepAlive(channel)
	runtime.KeepAlive(checkpoint)
	runtime.KeepAlive(limits)
	runtime.KeepAlive(operation)
	return workspaceJSONResult(int(code), ptr, length)
}
