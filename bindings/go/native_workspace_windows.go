//go:build windows && (amd64 || arm64)

package syrnck

import (
	"errors"
	"runtime"
	"unsafe"
)

func nativeWorkspaceNative(h nativeWorkspaceHandle, store nativeHandle, mode uint32, arguments [6][]byte, cancellation nativeCancellationHandle) ([]byte, error) {
	// Keep actual pointers in the expected-state struct so Go tracks its inputs.
	// Convert output/operation addresses inside each Proc.Call expression: storing
	// uintptr addresses in an argument slice hides them from stack relocation.
	// See https://pkg.go.dev/unsafe#Pointer, syscall conversion rule (4).
	expected := [3]*byte{unsafe.SliceData(arguments[0]), unsafe.SliceData(arguments[1]), unsafe.SliceData(arguments[5])}
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	var ptr, length uintptr
	var code uintptr
	switch mode {
	case 0:
		code, _, _ = library.functions["workspace_native_state_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	case 1:
		code, _, _ = library.functions["workspace_capture_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(&expected)), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[4]))), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	case 2:
		code, _, _ = library.functions["workspace_checkout_plan_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(&expected)), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[2]))), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	case 3:
		code, _, _ = library.functions["workspace_checkout_owned_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(&expected)), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[2]))), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[3]))), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	case 4:
		code, _, _ = library.functions["workspace_recover_owned_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[0]))), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[2]))), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[3]))), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	case 6:
		code, _, _ = library.functions["workspace_recover_owned_preserving_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[0]))), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[2]))), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[3]))), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	case 5:
		code, _, _ = library.functions["workspace_checkout_status_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(unsafe.SliceData(arguments[2]))), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	default:
		return nil, errors.New("invalid native workspace operation")
	}
	runtime.KeepAlive(arguments)
	runtime.KeepAlive(expected)
	runtime.KeepAlive(operation)
	return workspaceJSONResult(int(code), ptr, length)
}
