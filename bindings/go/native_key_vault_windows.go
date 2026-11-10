//go:build windows && (amd64 || arm64)

package syrnck

import (
	"errors"
	"runtime"
	"unsafe"
)

type nativeSystemVaultHandle uintptr

func nativeDisableMacOSKeychainInteraction() error {
	if err := loadLibrary(); err != nil {
		return err
	}
	code, _, _ := library.functions["system_vault_disable_macos_interaction"].Call()
	return status(int(code))
}

var vaultFunctions = []string{"system_vault_open", "system_vault_close", "system_vault_disable_macos_interaction", "system_vault_provider_json",
	"system_vault_generate", "system_vault_import_protected", "system_vault_open_secret",
	"system_vault_open_signer", "system_vault_open_sealed", "system_vault_export_protected", "system_vault_erase"}

func nativeOpenSystemVault(path []byte) (nativeSystemVaultHandle, error) {
	if err := loadLibrary(); err != nil {
		return 0, err
	}
	var result nativeSystemVaultHandle
	code, _, _ := library.functions["system_vault_open"].Call(uintptr(unsafe.Pointer(unsafe.SliceData(path))), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(path)
	return result, status(int(code))
}
func nativeCloseSystemVault(handle nativeSystemVaultHandle) error {
	code, _, _ := library.functions["system_vault_close"].Call(uintptr(handle))
	return status(int(code))
}
func nativeSystemVaultProvider(handle nativeSystemVaultHandle) ([]byte, error) {
	var result, length uintptr
	code, _, _ := library.functions["system_vault_provider_json"].Call(uintptr(handle), uintptr(unsafe.Pointer(&result)), uintptr(unsafe.Pointer(&length)))
	return workspaceJSONResult(int(code), result, length)
}
func vaultReferenceResult(data []byte, count uintptr, code int) (VaultReference, error) {
	if err := status(code); err != nil {
		return "", err
	}
	if count == 0 || count >= uintptr(len(data)) || data[count] != 0 {
		return "", errors.New("invalid native system vault reference length")
	}
	return ParseVaultReference(string(data[:count]))
}
func nativeSystemVaultGenerate(handle nativeSystemVaultHandle, purpose VaultKeyPurpose) (VaultReference, error) {
	var out [97]byte
	var count uintptr
	code, _, _ := library.functions["system_vault_generate"].Call(uintptr(handle), uintptr(purpose), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(out)
	return vaultReferenceResult(out[:], count, int(code))
}
func nativeSystemVaultImport(handle nativeSystemVaultHandle, purpose VaultKeyPurpose, envelope, password []byte) (VaultReference, error) {
	var out [97]byte
	var count uintptr
	code, _, _ := library.functions["system_vault_import_protected"].Call(uintptr(handle), uintptr(purpose), uintptr(unsafe.Pointer(unsafe.SliceData(envelope))), uintptr(len(envelope)), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(envelope)
	runtime.KeepAlive(password)
	runtime.KeepAlive(out)
	return vaultReferenceResult(out[:], count, int(code))
}
func nativeVaultKey(name string, handle nativeSystemVaultHandle, reference []byte) (uintptr, error) {
	var result uintptr
	code, _, _ := library.functions[name].Call(uintptr(handle), uintptr(unsafe.Pointer(unsafe.SliceData(reference))), uintptr(unsafe.Pointer(&result)))
	runtime.KeepAlive(reference)
	return result, status(int(code))
}
func nativeSystemVaultOpenSecret(handle nativeSystemVaultHandle, reference []byte) (nativeSecretKeyHandle, error) {
	result, err := nativeVaultKey("system_vault_open_secret", handle, reference)
	return nativeSecretKeyHandle(result), err
}
func nativeSystemVaultOpenSigner(handle nativeSystemVaultHandle, reference []byte) (nativeSigningKeyHandle, error) {
	result, err := nativeVaultKey("system_vault_open_signer", handle, reference)
	return nativeSigningKeyHandle(result), err
}
func nativeSystemVaultOpenSealed(handle nativeSystemVaultHandle, reference []byte) (nativeSealedKeyHandle, error) {
	result, err := nativeVaultKey("system_vault_open_sealed", handle, reference)
	return nativeSealedKeyHandle(result), err
}
func nativeSystemVaultExport(handle nativeSystemVaultHandle, reference, password []byte, costs *[2]uint32) ([]byte, error) {
	out := make([]byte, ProtectedKeySize)
	var count uintptr
	code, _, _ := library.functions["system_vault_export_protected"].Call(uintptr(handle), uintptr(unsafe.Pointer(unsafe.SliceData(reference))), uintptr(unsafe.Pointer(unsafe.SliceData(password))), uintptr(len(password)), uintptr(unsafe.Pointer(costs)), uintptr(unsafe.Pointer(&out[0])), uintptr(len(out)), uintptr(unsafe.Pointer(&count)))
	runtime.KeepAlive(reference)
	runtime.KeepAlive(password)
	runtime.KeepAlive(costs)
	runtime.KeepAlive(out)
	return keyOutput(out, count, int(code))
}
func nativeSystemVaultErase(handle nativeSystemVaultHandle, reference []byte) (bool, error) {
	var removed uint8
	code, _, _ := library.functions["system_vault_erase"].Call(uintptr(handle), uintptr(unsafe.Pointer(unsafe.SliceData(reference))), uintptr(unsafe.Pointer(&removed)))
	runtime.KeepAlive(reference)
	return removed != 0, status(int(code))
}
