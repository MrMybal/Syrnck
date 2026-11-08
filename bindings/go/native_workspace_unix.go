//go:build (linux || darwin) && cgo

package syrnck

/*
#include <stdlib.h>
#include <syrnck.h>
*/
import "C"
import "unsafe"

func nativeWorkspaceNative(h nativeWorkspaceHandle, store nativeHandle, mode uint32, arguments [6][]byte, cancellation nativeCancellationHandle) ([]byte, error) {
	// The expected-state struct contains pointers. Its strings are C-owned so
	// passing the struct never passes a Go pointer to another Go pointer.
	var pointers [6]*C.char
	for i, data := range arguments {
		if data != nil {
			pointers[i] = (*C.char)(C.CBytes(data))
			defer C.free(unsafe.Pointer(pointers[i]))
		}
	}
	expected := C.SyrnckNativeExpected{target: pointers[0], revision: pointers[1], materialization: pointers[5]}
	operation := C.SyrnckOperation{cancellation: cancellation}
	var ptr *C.char
	var length C.size_t
	var code C.int
	switch mode {
	case 0:
		code = C.syrnck_workspace_native_state_json_ex(h, store, &operation, &ptr, &length)
	case 1:
		code = C.syrnck_workspace_capture_json_ex(h, store, &expected, pointers[4], &operation, &ptr, &length)
	case 2:
		code = C.syrnck_workspace_checkout_plan_json_ex(h, store, &expected, pointers[2], &operation, &ptr, &length)
	case 3:
		code = C.syrnck_workspace_checkout_owned_json_ex(h, store, &expected, pointers[2], pointers[3], &operation, &ptr, &length)
	case 4:
		code = C.syrnck_workspace_recover_owned_json_ex(h, store, pointers[0], pointers[2], pointers[3], &operation, &ptr, &length)
	}
	return workspaceJSONResult(int(code), ptr, length)
}
