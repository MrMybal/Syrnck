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
