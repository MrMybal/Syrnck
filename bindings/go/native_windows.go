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
		for _, name := range []string{"abi_version", "error_copy", "store_open", "store_close", "ingest_file_ex", "restore_file", "file_verify", "restore_file_range", "file_read_range", "file_compare_json", "object_has", "object_read", "object_import", "closure_json", "ref_set", "ref_remove", "store_verify"} {
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
