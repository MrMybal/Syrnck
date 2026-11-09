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

type nativeSigningKeyHandle = *C.SyrnckSigningKey

func nativeProtectSigningKey(key, password []byte, costs *[2]uint32) ([]byte, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	out := make([]byte, ProtectedKeySize)
	var count C.size_t
	code := C.syrnck_signing_key_protect((*C.uchar)(unsafe.Pointer(unsafe.SliceData(key))), C.size_t(len(key)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(password))), C.size_t(len(password)), (*C.SyrnckPasswordKdf)(unsafe.Pointer(costs)), (*C.uchar)(unsafe.Pointer(&out[0])), C.size_t(len(out)), &count)
	if err := status(int(code)); err != nil {
		return nil, err
	}
	if count != ProtectedKeySize {
		return nil, errors.New("invalid native protected key length")
	}
	return out, nil
}
func nativeChangeSigningKeyPassword(envelope, before, after []byte, costs *[2]uint32) ([]byte, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	out := make([]byte, ProtectedKeySize)
	var count C.size_t
	code := C.syrnck_signing_key_change_password((*C.uchar)(unsafe.Pointer(unsafe.SliceData(envelope))), C.size_t(len(envelope)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(before))), C.size_t(len(before)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(after))), C.size_t(len(after)), (*C.SyrnckPasswordKdf)(unsafe.Pointer(costs)), (*C.uchar)(unsafe.Pointer(&out[0])), C.size_t(len(out)), &count)
	if err := status(int(code)); err != nil {
		return nil, err
	}
	if count != ProtectedKeySize {
		return nil, errors.New("invalid native protected key length")
	}
	return out, nil
}
func nativeOpenSigner(envelope, password []byte) (nativeSigningKeyHandle, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	var result nativeSigningKeyHandle
	code := C.syrnck_signing_key_open((*C.uchar)(unsafe.Pointer(unsafe.SliceData(envelope))), C.size_t(len(envelope)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(password))), C.size_t(len(password)), &result)
	return result, status(int(code))
}
func nativeCloseSigner(key nativeSigningKeyHandle) error {
	return status(int(C.syrnck_signing_key_close(key)))
}
func nativeSignerMetadata(key nativeSigningKeyHandle) ([]byte, error) {
	var ptr *C.char
	var length C.size_t
	code := C.syrnck_signing_key_public_json(key, &ptr, &length)
	return workspaceJSONResult(int(code), ptr, length)
}
func nativeUseSigner(h nativeHandle, name, email []byte, offset int32, key nativeSigningKeyHandle, required bool) error {
	return status(int(C.syrnck_store_use_native_signer(h, ctext(name), ctext(email), C.int32_t(offset), key, C.bool(required))))
}
