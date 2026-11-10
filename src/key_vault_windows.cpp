#include "key_vault_backend.hpp"
#include <Windows.h>
#include <wincred.h>
#include <memory>

namespace syrnck::detail {
namespace {
std::wstring target(const VaultKeyReference &ref) {
  const auto name = "org.syrnck.keys/" + ref.encode();
  return {name.begin(), name.end()};
}
struct FreeCredential {
  void operator()(CREDENTIALW *value) const noexcept {
    if (value->CredentialBlob) wipe_secret({value->CredentialBlob, value->CredentialBlobSize});
    CredFree(value);
  }
};
}
std::string_view vault_provider() noexcept { return "windows-credential-manager"; }
void vault_disable_macos_interaction() {}
std::optional<Bytes> vault_read(const std::optional<fs::path> &, const VaultKeyReference &ref) {
  CREDENTIALW *raw = nullptr;
  if (!CredReadW(target(ref).c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
    const auto error = GetLastError();
    if (error == ERROR_NOT_FOUND) return {};
    throw Error("cannot read system key vault: " + std::to_string(error));
  }
  const std::unique_ptr<CREDENTIALW, FreeCredential> credential(raw);
  require(raw->CredentialBlobSize == vault_record_size && raw->CredentialBlob,
          "invalid system key vault record");
  return Bytes(raw->CredentialBlob, raw->CredentialBlob + raw->CredentialBlobSize);
}
void vault_write(const std::optional<fs::path> &, const VaultKeyReference &ref,
    std::span<const std::uint8_t> record) {
  auto name = target(ref);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = name.data();
  credential.CredentialBlobSize = static_cast<DWORD>(record.size());
  credential.CredentialBlob = const_cast<BYTE *>(record.data());
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE; // Current account, local host; no enterprise roaming.
  if (!CredWriteW(&credential, 0))
    throw Error("cannot write system key vault: " + std::to_string(GetLastError()));
}
bool vault_erase(const std::optional<fs::path> &, const VaultKeyReference &ref) {
  if (CredDeleteW(target(ref).c_str(), CRED_TYPE_GENERIC, 0)) return true;
  const auto error = GetLastError();
  if (error == ERROR_NOT_FOUND) return false;
  throw Error("cannot erase system key vault entry: " + std::to_string(error));
}
} // namespace syrnck::detail
