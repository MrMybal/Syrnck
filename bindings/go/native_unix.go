//go:build (linux || darwin) && cgo

package syrnck

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -lsyrnck_ffi
#include <syrnck.h>
*/
import "C"

import (
	"errors"
	"unsafe"
)

type nativeHandle = *C.SyrnckStore

func nativeError() string    { return C.GoString(C.syrnck_last_error()) }
func ctext(s []byte) *C.char { return (*C.char)(unsafe.Pointer(unsafe.SliceData(s))) }

func nativeOpen(path []byte, create bool, key []byte) (nativeHandle, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	var result nativeHandle
	err := status(int(C.syrnck_store_open(ctext(path), C.bool(create), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(key))), C.size_t(len(key)), &result)))
	return result, err
}
func nativeClose(h nativeHandle) error { return status(int(C.syrnck_store_close(h))) }
func nativeIngest(h nativeHandle, path []byte, config *[4]uint32) (string, error) {
	var id [65]byte
	err := status(int(C.syrnck_ingest_file_ex(h, ctext(path), (*C.SyrnckChunking)(unsafe.Pointer(config)), (*C.char)(unsafe.Pointer(&id[0])))))
	return string(id[:64]), err
}
func nativeRestore(h nativeHandle, id, path []byte) error {
	return status(int(C.syrnck_restore_file(h, ctext(id), ctext(path))))
}
func nativeVerifyFile(h nativeHandle, id []byte) error {
	return status(int(C.syrnck_file_verify(h, ctext(id))))
}
func nativeRestoreRange(h nativeHandle, id, path []byte, offset, length uint64) error {
	return status(int(C.syrnck_restore_file_range(h, ctext(id), ctext(path), C.uint64_t(offset), C.uint64_t(length))))
}
func nativeRange(h nativeHandle, id []byte, offset, length uint64) ([]byte, error) {
	size := int(length)
	if size == 0 {
		size = 1
	}
	data := make([]byte, size)
	var count C.size_t
	err := status(int(C.syrnck_file_read_range(h, ctext(id), C.uint64_t(offset), C.uint64_t(length), (*C.uchar)(unsafe.Pointer(&data[0])), C.size_t(len(data)), &count)))
	if err != nil {
		return nil, err
	}
	if uint64(count) != length {
		return nil, errors.New("invalid native range length")
	}
	return data[:int(length)], nil
}
func nativeCompare(h nativeHandle, source, target []byte) ([]byte, error) {
	var length C.size_t
	call := func(data []byte) error {
		return status(int(C.syrnck_file_compare_json(h, ctext(source), ctext(target), (*C.char)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)), &length)))
	}
	if err := call(nil); err != nil {
		return nil, err
	}
	if length == 0 || length > 128*1024*1024 {
		return nil, errors.New("invalid native comparison size")
	}
	data := make([]byte, int(length))
	if err := call(data); err != nil {
		return nil, err
	}
	if int(length) != len(data) || data[len(data)-1] != 0 {
		return nil, errors.New("invalid native comparison length")
	}
	return data, nil
}

func nativeHas(h nativeHandle, kind, id []byte) (bool, error) {
	var present C.int
	err := status(int(C.syrnck_object_has(h, ctext(kind), ctext(id), &present)))
	return present != 0, err
}
func nativeRead(h nativeHandle, kind, id []byte, closure bool) ([]byte, error) {
	var length C.size_t
	read := func(data []byte) error {
		ptr := unsafe.Pointer(unsafe.SliceData(data))
		if closure {
			return status(int(C.syrnck_closure_json(h, ctext(kind), ctext(id), (*C.char)(ptr), C.size_t(len(data)), &length)))
		}
		return status(int(C.syrnck_object_read(h, ctext(kind), ctext(id), (*C.uchar)(ptr), C.size_t(len(data)), &length)))
	}
	if err := read(nil); err != nil {
		return nil, err
	}
	limit := C.size_t(MaxObjectSize)
	if closure {
		limit = 128 * 1024 * 1024
	}
	if length == 0 || length > limit {
		return nil, errors.New("invalid native output size")
	}
	data := make([]byte, int(length))
	err := read(data)
	return data, err
}
func nativeImport(h nativeHandle, kind, id, data []byte) (bool, error) {
	var created C.int
	err := status(int(C.syrnck_object_import(h, ctext(kind), ctext(id), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)), &created)))
	return created != 0, err
}
func nativePin(h nativeHandle, name, kind, id []byte) error {
	return status(int(C.syrnck_ref_set(h, ctext(name), ctext(kind), ctext(id))))
}
func nativeUnpin(h nativeHandle, name []byte) error {
	return status(int(C.syrnck_ref_remove(h, ctext(name))))
}
func nativeVerify(h nativeHandle) (uint64, error) {
	var objects C.size_t
	err := status(int(C.syrnck_store_verify(h, &objects)))
	return uint64(objects), err
}

type nativeWorkspaceHandle = *C.SyrnckWorkspace

func nativeWorkspaceOpen(root, store, name []byte, config *[3]uint32, cancellation nativeCancellationHandle) (nativeWorkspaceHandle, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	var result nativeWorkspaceHandle
	err := status(int(C.syrnck_workspace_open_registered_ex(ctext(root), ctext(store), ctext(name), (*C.SyrnckWorkspaceOptions)(unsafe.Pointer(config)), cancellation, &result)))
	return result, err
}
func nativeUseWorkspace(h nativeHandle, name []byte) error {
	return status(int(C.syrnck_store_use_workspace(h, ctext(name))))
}
func nativeWorkspaceClose(h nativeWorkspaceHandle) error {
	return status(int(C.syrnck_workspace_close(h)))
}
func nativeWorkspaceInvalidate(h nativeWorkspaceHandle) error {
	return status(int(C.syrnck_workspace_invalidate(h)))
}
func workspaceJSONResult(code int, ptr *C.char, length C.size_t) ([]byte, error) {
	err := status(code)
	if ptr != nil {
		defer C.syrnck_string_free(ptr)
	}
	if err != nil {
		return nil, err
	}
	if ptr == nil || length == 0 || length > 128*1024*1024+1 {
		return nil, errors.New("invalid native workspace output size")
	}
	data := C.GoBytes(unsafe.Pointer(ptr), C.int(length))
	if data[len(data)-1] != 0 {
		return nil, errors.New("invalid native workspace output terminator")
	}
	return data[:len(data)-1], nil
}
func nativeWorkspaceWait(h nativeWorkspaceHandle, timeout uint32, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr *C.char
	var length C.size_t
	code := int(C.syrnck_workspace_wait_json_ex(h, C.uint32_t(timeout), cancellation, &ptr, &length))
	return workspaceJSONResult(code, ptr, length)
}
func nativeWorkspaceStatus(h nativeWorkspaceHandle, store nativeHandle, selector []byte, mode uint32, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr *C.char
	var length C.size_t
	operation := C.SyrnckOperation{cancellation: cancellation}
	code := int(C.syrnck_workspace_status_json_ex(h, store, ctext(selector), C.uint32_t(mode), &operation, &ptr, &length))
	return workspaceJSONResult(code, ptr, length)
}

type nativeCancellationHandle = *C.SyrnckCancellation

func nativeCancellationCreate() (nativeCancellationHandle, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	var result nativeCancellationHandle
	err := status(int(C.syrnck_cancellation_create(&result)))
	return result, err
}
func nativeCancellationRequest(h nativeCancellationHandle) error {
	return status(int(C.syrnck_cancellation_request(h)))
}
func nativeCancellationClose(h nativeCancellationHandle) error {
	return status(int(C.syrnck_cancellation_close(h)))
}
