#include "key_vault_backend.hpp"
#include <libsecret/secret.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace syrnck::detail {
namespace {
const SecretSchema schema = [] {
  SecretSchema value{};
  value.name = "org.syrnck.keys.v1";
  value.flags = SECRET_SCHEMA_NONE;
  value.attributes[0] = {"reference", SECRET_SCHEMA_ATTRIBUTE_STRING};
  return value;
}();
template<class T> using Object = std::unique_ptr<T, decltype(&g_object_unref)>;
struct ErrorState {
  GError *value = nullptr;
  ~ErrorState() { if (value) g_error_free(value); }
  void check(std::string_view operation) const {
    if (value) throw Error(std::string(operation) + std::to_string(value->code));
  }
};
// The synchronous libsecret API otherwise permits unbounded prompt/DBus waits.
// One deadline covers the complete operation, including session establishment.
struct Call {
  Object<GCancellable> cancel{g_cancellable_new(), &g_object_unref};
  std::mutex mutex;
  std::condition_variable changed;
  bool finished = false;
  std::thread timer{[this] {
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(30), [this] { return finished; }))
      g_cancellable_cancel(cancel.get());
  }};
  Call() = default;
  Call(const Call &) = delete;
  Call &operator=(const Call &) = delete;
  ~Call() {
    { const std::lock_guard lock(mutex); finished = true; }
    changed.notify_one(); timer.join();
  }
};
Object<SecretService> service(Call &call, ErrorState &error) {
  Object<SecretService> value{secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, call.cancel.get(), &error.value),
                             &g_object_unref};
  error.check("cannot open system key vault: ");
  require(value != nullptr, "system key vault session is unavailable");
  return value;
}
struct Attributes {
  GHashTable *value;
  explicit Attributes(const VaultKeyReference &ref)
      : value(secret_attributes_build(&schema, "reference", ref.encode().c_str(), nullptr)) {}
  ~Attributes() { g_hash_table_unref(value); }
};
struct Items {
  GList *value;
  ~Items() { g_list_free_full(value, g_object_unref); }
};
Object<SecretItem> find(SecretService *vault, const VaultKeyReference &ref, Call &call, ErrorState &error) {
  const Attributes attributes(ref);
  const Items items{secret_service_search_sync(vault, &schema, attributes.value,
      static_cast<SecretSearchFlags>(SECRET_SEARCH_ALL | SECRET_SEARCH_LOAD_SECRETS), call.cancel.get(), &error.value)};
  error.check("cannot read system key vault: ");
  if (!items.value) return {nullptr, &g_object_unref};
  require(items.value->next == nullptr, "system key vault reference matches multiple entries");
  auto *item = static_cast<SecretItem *>(items.value->data);
  require(!secret_item_get_locked(item), "system key vault is locked");
  return {static_cast<SecretItem *>(g_object_ref(item)), &g_object_unref};
}
}
std::string_view vault_provider() noexcept { return "linux-secret-service"; }
std::optional<Bytes> vault_read(const std::optional<fs::path> &, const VaultKeyReference &ref) {
  Call call; ErrorState error;
  const auto vault = service(call, error);
  const auto item = find(vault.get(), ref, call, error);
  if (!item) return {};
  const std::unique_ptr<SecretValue, decltype(&secret_value_unref)> value{secret_item_get_secret(item.get()), &secret_value_unref};
  require(value != nullptr, "system key vault secret is unavailable");
  gsize size = 0;
  const auto raw = secret_value_get(value.get(), &size);
  require(raw && size == vault_record_size, "invalid system key vault record");
  const auto data = reinterpret_cast<const std::uint8_t *>(raw);
  return Bytes(data, data + size);
}
void vault_write(const std::optional<fs::path> &, const VaultKeyReference &ref,
    std::span<const std::uint8_t> record) {
  Call call; ErrorState error;
  const auto vault = service(call, error);
  const Object<SecretCollection> collection{secret_collection_for_alias_sync(vault.get(), SECRET_COLLECTION_DEFAULT,
      SECRET_COLLECTION_NONE, call.cancel.get(), &error.value), &g_object_unref};
  error.check("cannot open system key vault: ");
  require(collection != nullptr, "system key vault default collection is unavailable");
  require(!secret_collection_get_locked(collection.get()), "system key vault is locked");
  const Attributes attributes(ref);
  const std::unique_ptr<SecretValue, decltype(&secret_value_unref)> value{
      secret_value_new(reinterpret_cast<const char *>(record.data()), static_cast<gssize>(record.size()),
                       "application/octet-stream"), &secret_value_unref};
  const Object<SecretItem> created{secret_item_create_sync(collection.get(), &schema, attributes.value,
      "Syrnck key", value.get(), SECRET_ITEM_CREATE_NONE, call.cancel.get(), &error.value), &g_object_unref};
  error.check("cannot write system key vault: ");
  require(created != nullptr, "system key vault did not create an entry");
}
bool vault_erase(const std::optional<fs::path> &, const VaultKeyReference &ref) {
  Call call; ErrorState error;
  const auto vault = service(call, error);
  const auto item = find(vault.get(), ref, call, error);
  if (!item) return false;
  const auto removed = secret_item_delete_sync(item.get(), call.cancel.get(), &error.value);
  error.check("cannot erase system key vault entry: ");
  require(removed != FALSE, "system key vault did not erase an entry");
  return true;
}
} // namespace syrnck::detail
