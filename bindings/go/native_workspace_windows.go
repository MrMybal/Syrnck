//go:build windows && (amd64 || arm64)

package syrnck

import (
	"runtime"
	"unsafe"
)

func nativeWorkspaceNative(h nativeWorkspaceHandle, store nativeHandle, mode uint32, arguments [6][]byte, cancellation nativeCancellationHandle) ([]byte, error) {
	var pointers [6]uintptr
	for i, data := range arguments {
		pointers[i] = uintptr(unsafe.Pointer(unsafe.SliceData(data)))
	}
	expected := [3]uintptr{pointers[0], pointers[1], pointers[5]}
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	var ptr, length uintptr
	base := []uintptr{uintptr(h), uintptr(store)}
	op := uintptr(unsafe.Pointer(&operation))
	out := uintptr(unsafe.Pointer(&ptr))
	size := uintptr(unsafe.Pointer(&length))
	var name string
	var args []uintptr
	switch mode {
	case 0:
		name = "workspace_native_state_json_ex"
		args = append(base, op, out, size)
	case 1:
		name = "workspace_capture_json_ex"
		args = append(base, uintptr(unsafe.Pointer(&expected)), pointers[4], op, out, size)
	case 2:
		name = "workspace_checkout_plan_json_ex"
		args = append(base, uintptr(unsafe.Pointer(&expected)), pointers[2], op, out, size)
	case 3:
		name = "workspace_checkout_owned_json_ex"
		args = append(base, uintptr(unsafe.Pointer(&expected)), pointers[2], pointers[3], op, out, size)
	case 4:
		name = "workspace_recover_owned_json_ex"
		args = append(base, pointers[0], pointers[2], pointers[3], op, out, size)
	case 6:
		name = "workspace_recover_owned_preserving_json_ex"
		args = append(base, pointers[0], pointers[2], pointers[3], op, out, size)
	case 5:
		name = "workspace_checkout_status_json_ex"
		args = append(base, pointers[2], op, out, size)
	}
	r, _, _ := library.functions[name].Call(args...)
	runtime.KeepAlive(arguments)
	runtime.KeepAlive(expected)
	runtime.KeepAlive(operation)
	return workspaceJSONResult(int(r), ptr, length)
}
