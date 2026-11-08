//go:build (linux || darwin) && cgo

package syrnck

/*
#include <syrnck.h>
*/
import "C"

import (
	"errors"
	"unsafe"
)

func nativePut(h nativeHandle, kind, data []byte) (string, error) {
	var id [65]C.char
	err := status(int(C.syrnck_object_put(h, ctext(kind), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)), &id[0])))
	if err != nil {
		return "", err
	}
	return C.GoStringN(&id[0], 64), nil
}
func nativePrepareBuffer(h nativeHandle, data []byte, chunking *[4]uint32, cancellation nativeCancellationHandle) (string, error) {
	var id [65]C.char
	operation := C.SyrnckOperation{cancellation: cancellation}
	err := status(int(C.syrnck_ingest_buffer_ex(h, (*C.uchar)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)),
		(*C.SyrnckChunking)(unsafe.Pointer(chunking)), &operation, &id[0])))
	if err != nil {
		return "", err
	}
	return C.GoStringN(&id[0], 64), nil
}
func nativePrepareFile(h nativeHandle, path []byte, chunking *[4]uint32, cancellation nativeCancellationHandle) (string, error) {
	var id [65]C.char
	operation := C.SyrnckOperation{cancellation: cancellation}
	err := status(int(C.syrnck_prepare_file_ex(h, ctext(path), (*C.SyrnckChunking)(unsafe.Pointer(chunking)), &operation, &id[0])))
	if err != nil {
		return "", err
	}
	return C.GoStringN(&id[0], 64), nil
}
func nativeRefs(h nativeHandle) ([]byte, error) {
	var length C.size_t
	read := func(data []byte) error {
		return status(int(C.syrnck_refs_json(h, (*C.char)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)), &length)))
	}
	if err := read(nil); err != nil {
		return nil, err
	}
	if length < 1 || length > MaxObjectSize+1 {
		return nil, errors.New("invalid native reference table size")
	}
	data := make([]byte, int(length))
	if err := read(data); err != nil {
		return nil, err
	}
	if length != C.size_t(len(data)) || data[len(data)-1] != 0 {
		return nil, errors.New("invalid native reference table length")
	}
	return data[:len(data)-1], nil
}
func nativeCompareRefs(h nativeHandle, data []byte, cancellation nativeCancellationHandle) (bool, error) {
	var applied C.int
	operation := C.SyrnckOperation{cancellation: cancellation}
	err := status(int(C.syrnck_refs_compare_json(h, (*C.uchar)(unsafe.Pointer(unsafe.SliceData(data))), C.size_t(len(data)), &operation, &applied)))
	return applied != 0, err
}
