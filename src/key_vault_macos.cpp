#include "key_vault_backend.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace syrnck::detail {
namespace {
template<class T> struct CF {
  T value = nullptr;
  explicit CF(T object = nullptr) : value(object) {}
  CF(const CF &) = delete;
  CF &operator=(const CF &) = delete;
  ~CF() { if (value) CFRelease(value); }
};
struct Query {
  CF<CFMutableDictionaryRef> value{CFDictionaryCreateMutable(nullptr, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
  explicit Query(const std::optional<fs::path> &keychain, const VaultKeyReference &ref, bool writing) {
    require(value.value != nullptr, "cannot allocate system key vault query");
    const auto encoded = ref.encode();
    const CF<CFStringRef> account{CFStringCreateWithBytes(nullptr,
        reinterpret_cast<const UInt8 *>(encoded.data()), static_cast<CFIndex>(encoded.size()), kCFStringEncodingUTF8, false)};
    require(account.value != nullptr, "cannot allocate system key vault query");
    CFDictionarySetValue(value.value, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(value.value, kSecAttrService, CFSTR("org.syrnck.keys.v1"));
    CFDictionarySetValue(value.value, kSecAttrAccount, account.value);
    CFDictionarySetValue(value.value, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    if (keychain) {
      SecKeychainRef raw = nullptr;
      const auto status = SecKeychainOpen(keychain->c_str(), &raw);
      const CF<SecKeychainRef> selected{raw};
      if (status != errSecSuccess) throw Error("cannot open system key vault keychain: " + std::to_string(status));
      if (writing) CFDictionarySetValue(value.value, kSecUseKeychain, selected.value);
      else {
        const void *item = selected.value;
        const CF<CFArrayRef> list{CFArrayCreate(nullptr, &item, 1, &kCFTypeArrayCallBacks)};
        require(list.value != nullptr, "cannot allocate system key vault query");
        CFDictionarySetValue(value.value, kSecMatchSearchList, list.value);
      }
    }
  }
};
}
std::string_view vault_provider() noexcept { return "macos-keychain"; }
std::optional<Bytes> vault_read(const std::optional<fs::path> &keychain, const VaultKeyReference &ref) {
  Query query(keychain, ref, false);
  CFDictionarySetValue(query.value.value, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query.value.value, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef raw = nullptr;
  const auto status = SecItemCopyMatching(query.value.value, &raw);
  const CF<CFTypeRef> result{raw};
  if (status == errSecItemNotFound) return {};
  if (status != errSecSuccess) throw Error("cannot read system key vault: " + std::to_string(status));
  require(raw && CFGetTypeID(raw) == CFDataGetTypeID(), "invalid system key vault record");
  const auto data = static_cast<CFDataRef>(raw);
  require(CFDataGetLength(data) == static_cast<CFIndex>(vault_record_size), "invalid system key vault record");
  const auto bytes = CFDataGetBytePtr(data);
  return Bytes(bytes, bytes + vault_record_size);
}
void vault_write(const std::optional<fs::path> &keychain, const VaultKeyReference &ref,
    std::span<const std::uint8_t> record) {
  Query query(keychain, ref, true);
  // Borrow our bounded buffer for this synchronous call; the caller wipes it.
  const CF<CFDataRef> data{CFDataCreateWithBytesNoCopy(nullptr, record.data(),
      static_cast<CFIndex>(record.size()), kCFAllocatorNull)};
  require(data.value != nullptr, "cannot allocate system key vault query");
  CFDictionarySetValue(query.value.value, kSecValueData, data.value);
  const auto status = SecItemAdd(query.value.value, nullptr);
  if (status != errSecSuccess) throw Error("cannot write system key vault: " + std::to_string(status));
}
bool vault_erase(const std::optional<fs::path> &keychain, const VaultKeyReference &ref) {
  Query query(keychain, ref, false);
  const auto status = SecItemDelete(query.value.value);
  if (status == errSecItemNotFound) return false;
  if (status != errSecSuccess) throw Error("cannot erase system key vault entry: " + std::to_string(status));
  return true;
}
} // namespace syrnck::detail
