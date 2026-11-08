//go:build windows && (amd64 || arm64)

package syrnck

import (
	"errors"
	"runtime"
	"unsafe"
)

func nativePut(h nativeHandle, kind, data []byte) (string, error) {
	var id [65]byte
	r, _, _ := library.functions["object_put"].Call(uintptr(h), uintptr(unsafe.Pointer(&kind[0])),
		uintptr(unsafe.Pointer(unsafe.SliceData(data))), uintptr(len(data)), uintptr(unsafe.Pointer(&id[0])))
	runtime.KeepAlive(kind)
	runtime.KeepAlive(data)
	if err := status(int(r)); err != nil {
		return "", err
	}
	return string(id[:64]), nil
}
func nativePrepareBuffer(h nativeHandle, data []byte, chunking *[4]uint32, cancellation nativeCancellationHandle) (string, error) {
	var id [65]byte
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	r, _, _ := library.functions["ingest_buffer_ex"].Call(uintptr(h), uintptr(unsafe.Pointer(unsafe.SliceData(data))), uintptr(len(data)),
		uintptr(unsafe.Pointer(chunking)), uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&id[0])))
	runtime.KeepAlive(data)
	runtime.KeepAlive(chunking)
	runtime.KeepAlive(operation)
	if err := status(int(r)); err != nil {
		return "", err
	}
	return string(id[:64]), nil
}
func nativePrepareFile(h nativeHandle, path []byte, chunking *[4]uint32, cancellation nativeCancellationHandle) (string, error) {
	var id [65]byte
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	r, _, _ := library.functions["prepare_file_ex"].Call(uintptr(h), uintptr(unsafe.Pointer(&path[0])), uintptr(unsafe.Pointer(chunking)),
		uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&id[0])))
	runtime.KeepAlive(path)
	runtime.KeepAlive(chunking)
	runtime.KeepAlive(operation)
	if err := status(int(r)); err != nil {
		return "", err
	}
	return string(id[:64]), nil
}
func nativeRefs(h nativeHandle) ([]byte, error) {
	var length uintptr
	read := func(data []byte) error {
		r, _, _ := library.functions["refs_json"].Call(uintptr(h), uintptr(unsafe.Pointer(unsafe.SliceData(data))), uintptr(len(data)), uintptr(unsafe.Pointer(&length)))
		runtime.KeepAlive(data)
		return status(int(r))
	}
	if err := read(nil); err != nil {
		return nil, err
	}
	if length < 1 || length > MaxObjectSize+1 {
		return nil, errors.New("invalid native reference table size")
	}
	data := make([]byte, length)
	if err := read(data); err != nil {
		return nil, err
	}
	if length != uintptr(len(data)) || data[len(data)-1] != 0 {
		return nil, errors.New("invalid native reference table length")
	}
	return data[:len(data)-1], nil
}
func nativeCompareRefs(h nativeHandle, data []byte, cancellation nativeCancellationHandle) (bool, error) {
	var applied int32
	operation := [3]uintptr{uintptr(cancellation), 0, 0}
	r, _, _ := library.functions["refs_compare_json"].Call(uintptr(h), uintptr(unsafe.Pointer(unsafe.SliceData(data))), uintptr(len(data)),
		uintptr(unsafe.Pointer(&operation)), uintptr(unsafe.Pointer(&applied)))
	runtime.KeepAlive(data)
	runtime.KeepAlive(operation)
	return applied != 0, status(int(r))
}
