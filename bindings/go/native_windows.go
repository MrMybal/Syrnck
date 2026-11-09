//go:build windows && (amd64 || arm64)

package syrnck

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"unsafe"
)

type nativeHandle uintptr

var library struct {
	once      sync.Once
	err       error
	functions map[string]*syscall.Proc
	dll       *syscall.DLL // kept loaded for the process lifetime
}

func loadLibrary() error {
	library.once.Do(func() {
		path := os.Getenv("SYRNCK_LIBRARY")
		if !filepath.IsAbs(path) {
			library.err = errors.New("SYRNCK_LIBRARY must be an absolute path to syrnck_ffi.dll")
			return
		}
		library.dll, library.err = syscall.LoadDLL(path)
		if library.err != nil {
			return
		}
		library.functions = make(map[string]*syscall.Proc)
		for _, name := range []string{"sealed_key_protect", "sealed_key_change_password", "sealed_key_open", "sealed_key_close", "sealed_key_public_json", "sealed_snapshot_json_ex", "sealed_inspect_json_ex", "sealed_open_json_ex", "signing_key_protect", "signing_key_change_password", "signing_key_open", "signing_key_close", "signing_key_public_json", "store_use_native_signer", "key_protect", "key_change_password", "secret_key_open", "secret_key_close", "store_open_secret", "store_use_native_identity", "store_clear_native_identity", "store_native_identity_json", "prepare_native_revision_ex", "workspace_recover_owned_preserving_json_ex", "workspace_checkout_status_json_ex", "workspace_native_state_json_ex", "workspace_capture_json_ex", "workspace_checkout_plan_json_ex", "workspace_checkout_owned_json_ex", "workspace_recover_owned_json_ex", "object_put", "ingest_buffer_ex", "prepare_file_ex", "refs_json", "refs_compare_json", "abi_version", "error_copy", "store_open", "store_close", "store_use_workspace", "workspace_open_registered_ex", "ingest_file_ex", "restore_file", "file_verify", "restore_file_range", "file_read_range", "file_compare_json", "object_has", "object_read", "object_import", "closure_json", "ref_set", "ref_remove", "store_verify", "string_free", "workspace_open", "workspace_close", "workspace_wait_json", "workspace_status_json", "workspace_invalidate", "cancellation_create", "cancellation_request", "cancellation_close", "workspace_open_ex", "workspace_wait_json_ex", "workspace_status_json_ex"} {
			p, err := library.dll.FindProc("syrnck_" + name)
			if err != nil {
				library.err = err
				return
			}
			library.functions[name] = p
		}
		v, _, _ := library.functions["abi_version"].Call()
		if v != 1 {
			library.err = fmt.Errorf("unsupported Syrnck ABI %d", v)
		}
	})
	return library.err
}

func nativeError() string {
	var length uintptr
	library.functions["error_copy"].Call(0, 0, uintptr(unsafe.Pointer(&length)))
	if length == 0 || length > 1024*1024 {
		return "invalid native error length"
	}
	data := make([]byte, length)
	library.functions["error_copy"].Call(uintptr(unsafe.Pointer(&data[0])), length, uintptr(unsafe.Pointer(&length)))
	return string(data[:len(data)-1])
}

func nativeOpen(path []byte, create bool, key []byte) (nativeHandle, error) {
	if err := loadLibrary(); err != nil {
		return 0, err
	}
	var result nativeHandle
	var flag uintptr
	if create {
		flag = 1
	}
	r, _, _ := library.functions["store_open"].Call(uintptr(unsafe.Pointer(&path[0])), flag,
		uintptr(unsafe.Pointer(unsafe.SliceData(key))), uintptr(len(key)), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(path)
	runtime.KeepAlive(key)
	return result, status(int(r))
}

func nativeClose(h nativeHandle) error {
	r, _, _ := library.functions["store_close"].Call(uintptr(h))
	return status(int(r))
}

func nativeIngest(h nativeHandle, path []byte, config *[4]uint32) (string, error) {
	var id [65]byte
	r, _, _ := library.functions["ingest_file_ex"].Call(uintptr(h), uintptr(unsafe.Pointer(&path[0])), uintptr(unsafe.Pointer(config)), uintptr(unsafe.Pointer(&id[0])))
	runtime.KeepAlive(path)
	runtime.KeepAlive(config)
	return string(id[:64]), status(int(r))
}

func nativeRestore(h nativeHandle, id, path []byte) error {
	r, _, _ := library.functions["restore_file"].Call(uintptr(h), uintptr(unsafe.Pointer(&id[0])), uintptr(unsafe.Pointer(&path[0])))
	runtime.KeepAlive(id)
	runtime.KeepAlive(path)
	return status(int(r))
}
func nativeVerifyFile(h nativeHandle, id []byte) error {
	r, _, _ := library.functions["file_verify"].Call(uintptr(h), uintptr(unsafe.Pointer(&id[0])))
	runtime.KeepAlive(id)
	return status(int(r))
}

func nativeRestoreRange(h nativeHandle, id, path []byte, offset, length uint64) error {
	r, _, _ := library.functions["restore_file_range"].Call(uintptr(h), uintptr(unsafe.Pointer(&id[0])), uintptr(unsafe.Pointer(&path[0])), uintptr(offset), uintptr(length))
	runtime.KeepAlive(id)
	runtime.KeepAlive(path)
	return status(int(r))
}
func nativeRange(h nativeHandle, id []byte, offset, length uint64) ([]byte, error) {
	size := int(length)
	if size == 0 {
		size = 1
	}
	data := make([]byte, size)
	var count uintptr
	r, _, _ := library.functions["file_read_range"].Call(uintptr(h), uintptr(unsafe.Pointer(&id[0])), uintptr(offset), uintptr(length), uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(id)
	runtime.KeepAlive(data)
	if err := status(int(r)); err != nil {
		return nil, err
	}
	if uint64(count) != length {
		return nil, errors.New("invalid native range length")
	}
	return data[:int(length)], nil
}
func nativeCompare(h nativeHandle, source, target []byte) ([]byte, error) {
	defer runtime.KeepAlive(source)
	defer runtime.KeepAlive(target)
	var length uintptr
	call := func(data []byte) error {
		r, _, _ := library.functions["file_compare_json"].Call(uintptr(h), uintptr(unsafe.Pointer(&source[0])), uintptr(unsafe.Pointer(&target[0])), uintptr(unsafe.Pointer(unsafe.SliceData(data))), uintptr(len(data)), uintptr(unsafe.Pointer(&length)))
		runtime.KeepAlive(data)
		return status(int(r))
	}
	if err := call(nil); err != nil {
		return nil, err
	}
	if length == 0 || length > 128*1024*1024 {
		return nil, errors.New("invalid native comparison size")
	}
	data := make([]byte, length)
	if err := call(data); err != nil {
		return nil, err
	}
	if length != uintptr(len(data)) || data[len(data)-1] != 0 {
		return nil, errors.New("invalid native comparison length")
	}
	return data, nil
}

func nativeHas(h nativeHandle, kind, id []byte) (bool, error) {
	var present int32
	r, _, _ := library.functions["object_has"].Call(uintptr(h), uintptr(unsafe.Pointer(&kind[0])), uintptr(unsafe.Pointer(&id[0])), uintptr(unsafe.Pointer(&present)))
	runtime.KeepAlive(kind)
	runtime.KeepAlive(id)
	return present != 0, status(int(r))
}

func nativeRead(h nativeHandle, kind, id []byte, closure bool) ([]byte, error) {
	name := "object_read"
	limit := uintptr(MaxObjectSize)
	if closure {
		name = "closure_json"
		limit = 128 * 1024 * 1024
	}
	var length uintptr
	r, _, _ := library.functions[name].Call(uintptr(h), uintptr(unsafe.Pointer(&kind[0])), uintptr(unsafe.Pointer(&id[0])), 0, 0, uintptr(unsafe.Pointer(&length)))
	if err := status(int(r)); err != nil {
		return nil, err
	}
	if length == 0 || length > limit {
		return nil, errors.New("invalid native output size")
	}
	data := make([]byte, length)
	r, _, _ = library.functions[name].Call(uintptr(h), uintptr(unsafe.Pointer(&kind[0])), uintptr(unsafe.Pointer(&id[0])), uintptr(unsafe.Pointer(&data[0])), length, uintptr(unsafe.Pointer(&length)))
	runtime.KeepAlive(kind)
	runtime.KeepAlive(id)
	return data, status(int(r))
}

func nativeImport(h nativeHandle, kind, id, data []byte) (bool, error) {
	var created int32
	r, _, _ := library.functions["object_import"].Call(uintptr(h), uintptr(unsafe.Pointer(&kind[0])), uintptr(unsafe.Pointer(&id[0])), uintptr(unsafe.Pointer(unsafe.SliceData(data))), uintptr(len(data)), uintptr(unsafe.Pointer(&created)))
	runtime.KeepAlive(kind)
	runtime.KeepAlive(id)
	runtime.KeepAlive(data)
	return created != 0, status(int(r))
}

func nativePin(h nativeHandle, name, kind, id []byte) error {
	r, _, _ := library.functions["ref_set"].Call(uintptr(h), uintptr(unsafe.Pointer(&name[0])), uintptr(unsafe.Pointer(&kind[0])), uintptr(unsafe.Pointer(&id[0])))
	runtime.KeepAlive(name)
	runtime.KeepAlive(kind)
	runtime.KeepAlive(id)
	return status(int(r))
}

func nativeUnpin(h nativeHandle, name []byte) error {
	r, _, _ := library.functions["ref_remove"].Call(uintptr(h), uintptr(unsafe.Pointer(&name[0])))
	runtime.KeepAlive(name)
	return status(int(r))
}

func nativeVerify(h nativeHandle) (uint64, error) {
	var objects uintptr
	r, _, _ := library.functions["store_verify"].Call(uintptr(h), uintptr(unsafe.Pointer(&objects)))
	return uint64(objects), status(int(r))
}

type nativeWorkspaceHandle uintptr

func nativeWorkspaceOpen(root, store, name []byte, config *[3]uint32, cancellation nativeCancellationHandle) (nativeWorkspaceHandle, error) {
	if err := loadLibrary(); err != nil {
		return 0, err
	}
	var result nativeWorkspaceHandle
	r, _, _ := library.functions["workspace_open_registered_ex"].Call(uintptr(unsafe.Pointer(&root[0])), uintptr(unsafe.Pointer(&store[0])), uintptr(unsafe.Pointer(&name[0])), uintptr(unsafe.Pointer(config)), uintptr(cancellation), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(root)
	runtime.KeepAlive(store)
	runtime.KeepAlive(name)
	runtime.KeepAlive(config)
	return result, status(int(r))
}
func nativeUseWorkspace(h nativeHandle, name []byte) error {
	r, _, _ := library.functions["store_use_workspace"].Call(uintptr(h), uintptr(unsafe.Pointer(&name[0])))
	runtime.KeepAlive(name)
	return status(int(r))
}
func nativeWorkspaceClose(h nativeWorkspaceHandle) error {
	r, _, _ := library.functions["workspace_close"].Call(uintptr(h))
	return status(int(r))
}
func nativeWorkspaceInvalidate(h nativeWorkspaceHandle) error {
	r, _, _ := library.functions["workspace_invalidate"].Call(uintptr(h))
	return status(int(r))
}
func workspaceJSONResult(code int, ptr, length uintptr) ([]byte, error) {
	err := status(code) // Save thread-local diagnostics before freeing the result.
	if ptr != 0 {
		defer library.functions["string_free"].Call(ptr)
	}
	if err != nil {
		return nil, err
	}
	if ptr == 0 || length == 0 || length > 128*1024*1024+1 {
		return nil, errors.New("invalid native workspace output size")
	}
	data := unsafe.Slice((*byte)(unsafe.Pointer(ptr)), int(length))
	if data[len(data)-1] != 0 {
		return nil, errors.New("invalid native workspace output terminator")
	}
	return append([]byte(nil), data[:len(data)-1]...), nil
}
func nativeWorkspaceWait(h nativeWorkspaceHandle, timeout uint32, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr, length uintptr
	r, _, _ := library.functions["workspace_wait_json_ex"].Call(uintptr(h), uintptr(timeout), uintptr(cancellation), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	return workspaceJSONResult(int(r), ptr, length)
}
func nativeWorkspaceStatus(h nativeWorkspaceHandle, store nativeHandle, selector []byte, mode uint32, cancellation nativeCancellationHandle) ([]byte, error) {
	var ptr, length uintptr
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	r, _, _ := library.functions["workspace_status_json_ex"].Call(uintptr(h), uintptr(store), uintptr(unsafe.Pointer(unsafe.SliceData(selector))), uintptr(mode), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&ptr)), uintptr(unsafe.Pointer(&length)))
	runtime.KeepAlive(operation)
	runtime.KeepAlive(selector)
	return workspaceJSONResult(int(r), ptr, length)
}

type nativeCancellationHandle uintptr

func nativeCancellationCreate() (nativeCancellationHandle, error) {
	if err := loadLibrary(); err != nil {
		return 0, err
	}
	var result nativeCancellationHandle
	r, _, _ := library.functions["cancellation_create"].Call(uintptr(unsafe.Pointer(&result)))
	return result, status(int(r))
}
func nativeCancellationRequest(h nativeCancellationHandle) error {
	r, _, _ := library.functions["cancellation_request"].Call(uintptr(h))
	return status(int(r))
}
func nativeCancellationClose(h nativeCancellationHandle) error {
	r, _, _ := library.functions["cancellation_close"].Call(uintptr(h))
	return status(int(r))
}
