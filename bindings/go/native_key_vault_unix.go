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

type nativeSystemVaultHandle = *C.SyrnckSystemVault

func nativeDisableMacOSKeychainInteraction() error {
	return status(int(C.syrnck_system_vault_disable_macos_interaction()))
}

func nativeOpenSystemVault(path []byte) (nativeSystemVaultHandle, error) {
	if C.syrnck_abi_version() != 1 {
		return nil, errors.New("unsupported Syrnck ABI")
	}
	var result nativeSystemVaultHandle
	code := C.syrnck_system_vault_open(ctext(path), &result)
	return result, status(int(code))
}
func nativeCloseSystemVault(handle nativeSystemVaultHandle) error {
	return status(int(C.syrnck_system_vault_close(handle)))
}
func nativeSystemVaultProvider(handle nativeSystemVaultHandle) ([]byte, error) {
	var result *C.char
	var count C.size_t
	code := C.syrnck_system_vault_provider_json(handle, &result, &count)
	return workspaceJSONResult(int(code), result, count)
}
func vaultReferenceResult(data []byte, count C.size_t, code int) (VaultReference, error) {
	if err := status(code); err != nil {
		return "", err
	}
	if count == 0 || uint64(count) >= uint64(len(data)) || data[count] != 0 {
		return "", errors.New("invalid native system vault reference length")
	}
	return ParseVaultReference(string(data[:count]))
}
func nativeSystemVaultGenerate(handle nativeSystemVaultHandle, purpose VaultKeyPurpose) (VaultReference, error) {
	var out [C.SYRNCK_VAULT_REFERENCE_CAPACITY]byte
	var count C.size_t
	code := C.syrnck_system_vault_generate(handle, C.uint32_t(purpose), (*C.char)(unsafe.Pointer(&out[0])), C.size_t(len(out)), &count)
	return vaultReferenceResult(out[:], count, int(code))
}
func nativeSystemVaultImport(handle nativeSystemVaultHandle, purpose VaultKeyPurpose, envelope, password []byte) (VaultReference, error) {
	var out [C.SYRNCK_VAULT_REFERENCE_CAPACITY]byte
	var count C.size_t
	code := C.syrnck_system_vault_import_protected(handle, C.uint32_t(purpose), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(envelope))), C.size_t(len(envelope)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(password))), C.size_t(len(password)), (*C.char)(unsafe.Pointer(&out[0])), C.size_t(len(out)), &count)
	return vaultReferenceResult(out[:], count, int(code))
}
func nativeSystemVaultOpenSecret(handle nativeSystemVaultHandle, reference []byte) (nativeSecretKeyHandle, error) {
	var result nativeSecretKeyHandle
	code := C.syrnck_system_vault_open_secret(handle, ctext(reference), &result)
	return result, status(int(code))
}
func nativeSystemVaultOpenSigner(handle nativeSystemVaultHandle, reference []byte) (nativeSigningKeyHandle, error) {
	var result nativeSigningKeyHandle
	code := C.syrnck_system_vault_open_signer(handle, ctext(reference), &result)
	return result, status(int(code))
}
func nativeSystemVaultOpenSealed(handle nativeSystemVaultHandle, reference []byte) (nativeSealedKeyHandle, error) {
	var result nativeSealedKeyHandle
	code := C.syrnck_system_vault_open_sealed(handle, ctext(reference), &result)
	return result, status(int(code))
}
func nativeSystemVaultExport(handle nativeSystemVaultHandle, reference, password []byte, costs *[2]uint32) ([]byte, error) {
	out := make([]byte, ProtectedKeySize)
	var count C.size_t
	code := C.syrnck_system_vault_export_protected(handle, ctext(reference), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(password))), C.size_t(len(password)), (*C.SyrnckPasswordKdf)(unsafe.Pointer(costs)), (*C.uchar)(unsafe.Pointer(&out[0])), C.size_t(len(out)), &count)
	if err := status(int(code)); err != nil {
		return nil, err
	}
	if count != ProtectedKeySize {
		return nil, errors.New("invalid native protected key length")
	}
	return out, nil
}
func nativeSystemVaultErase(handle nativeSystemVaultHandle, reference []byte) (bool, error) {
	var removed C.bool
	code := C.syrnck_system_vault_erase(handle, ctext(reference), &removed)
	return bool(removed), status(int(code))
}
