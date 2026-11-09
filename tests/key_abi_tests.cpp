#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <iostream>
using namespace native_test;
namespace {
void success(int code) { require(code == 0, syrnck_last_error()); }
struct Secret {
  SyrnckSecretKey *value = nullptr;
  ~Secret() { syrnck_secret_key_close(value); }
};
struct Handle {
  SyrnckStore *value = nullptr;
  ~Handle() { syrnck_store_close(value); }
};
}
int main() {
  Temporary temporary;
  try {
    static_assert(sizeof(SyrnckPasswordKdf) == 8 && SYRNCK_PROTECTED_KEY_SIZE == 104);
    const Bytes password{'e',0,'x'}, replacement{'n',0,'w'}, wrong{'b','a','d'};
    Key raw; raw.fill(49);
    Bytes output(104, 0xa5), sentinel = output;
    std::size_t length = 999;
    success(syrnck_key_protect(raw.data(),raw.size(),password.data(),password.size(),nullptr,nullptr,0,&length));
    require(length == 104, "invalid protected key size query");
    require(syrnck_key_protect(raw.data(),raw.size(),password.data(),password.size(),nullptr,output.data(),103,&length) != 0 &&
        length == 104 && output == sentinel, "insufficient key output changed bytes");
    require(syrnck_key_protect(nullptr,32,password.data(),password.size(),nullptr,output.data(),104,&length) != 0 &&
        length == 0 && output == sentinel, "invalid key changed output");
    const SyrnckPasswordKdf invalid{0xffffffffU,3};
    require(syrnck_key_protect(raw.data(),raw.size(),password.data(),password.size(),&invalid,output.data(),104,&length) != 0 &&
        length == 0 && output == sentinel, "oversized KDF admitted or changed output");
    const SyrnckPasswordKdf zero{};
    success(syrnck_key_protect(raw.data(),raw.size(),password.data(),password.size(),&zero,output.data(),104,&length));
    const auto original = output;
    Secret key;
    require(syrnck_secret_key_open(output.data(),104,wrong.data(),wrong.size(),&key.value) != 0 && !key.value,
        "wrong key password returned a handle");
    success(syrnck_secret_key_open(output.data(),104,password.data(),password.size(),&key.value));
    Handle store;
    const auto path = utf8(temporary.path / "store");
    success(syrnck_store_open_secret(path.c_str(),true,key.value,&store.value));
    const auto payload = bytes("native-only key owner retained payload");
    char id[65]{};
    success(syrnck_object_put(store.value,"chunk",payload.data(),payload.size(),id));
    success(syrnck_ref_set(store.value,"retained","chunk",id));
    success(syrnck_secret_key_close(key.value)); key.value = nullptr;
    Bytes recovered(payload.size());
    success(syrnck_object_read(store.value,"chunk",id,recovered.data(),recovered.size(),&length));
    require(std::equal(recovered.begin(),recovered.end(),payload.begin()), "secret close invalidated Store");
    success(syrnck_store_close(store.value)); store.value = nullptr;
    output.assign(104,0xa5); sentinel = output;
    success(syrnck_key_change_password(original.data(),104,wrong.data(),wrong.size(),replacement.data(),replacement.size(),nullptr,nullptr,0,&length));
    require(length == 104, "rewrap size query falsely authenticated");
    require(syrnck_key_change_password(original.data(),104,wrong.data(),wrong.size(),replacement.data(),replacement.size(),nullptr,output.data(),104,&length) != 0 &&
        length == 0 && output == sentinel, "wrong rewrap password modified output");
    const SyrnckPasswordKdf partial{0,4};
    success(syrnck_key_change_password(original.data(),104,password.data(),password.size(),replacement.data(),replacement.size(),&partial,output.data(),104,&length));
    success(syrnck_secret_key_open(output.data(),104,replacement.data(),replacement.size(),&key.value));
    success(syrnck_store_open_secret(path.c_str(),false,key.value,&store.value));
    std::size_t objects=0;
    success(syrnck_store_verify(store.value,&objects));
    require(objects == 1 && original != output, "rewrap changed retained Store or reused envelope");
    success(syrnck_store_close(store.value)); store.value = nullptr;
    success(syrnck_store_open(path.c_str(),false,raw.data(),raw.size(),&store.value));
    success(syrnck_store_close(store.value)); store.value = nullptr;
    const auto refused = utf8(temporary.path / "no-store");
    require(syrnck_store_open_secret(refused.c_str(),true,nullptr,&store.value) != 0 && !store.value && !fs::exists(path_from_utf8(refused)),
        "missing native key created a Store");
    success(syrnck_secret_key_close(nullptr));
    length = 9;
    require(syrnck_key_protect(nullptr,0,nullptr,0,nullptr,nullptr,0,&length) != 0 && length == 0,
        "invalid password accepted by size query");
    Bytes sealed(104, 0xa5), untouched = sealed;
    success(syrnck_signing_key_protect(raw.data(), raw.size(), password.data(), password.size(), nullptr, nullptr, 0, &length));
    require(length == 104, "invalid protected signer query");
    require(syrnck_signing_key_protect(raw.data(), 31, password.data(), password.size(), nullptr, sealed.data(), sealed.size(), &length) != 0 &&
                length == 0 && sealed == untouched, "invalid signing seed wrote output");
    require(syrnck_signing_key_protect(raw.data(), raw.size(), password.data(), password.size(), nullptr, sealed.data(), 103, &length) != 0 &&
                length == 104 && sealed == untouched, "short signing output wrote bytes");
    success(syrnck_signing_key_protect(raw.data(), raw.size(), password.data(), password.size(), nullptr, sealed.data(), sealed.size(), &length));
    SyrnckSigningKey *signer = nullptr;
    struct SignerCleanup { SyrnckSigningKey *&value; ~SignerCleanup() { syrnck_signing_key_close(value); } } signer_cleanup{signer};
    require(syrnck_signing_key_open(original.data(), original.size(), password.data(), password.size(), &signer) != 0 && !signer,
            "storage envelope became a signing owner");
    require(syrnck_signing_key_open(sealed.data(), sealed.size(), wrong.data(), wrong.size(), &signer) != 0 && !signer,
            "wrong signing password returned an owner");
    success(syrnck_signing_key_open(sealed.data(), sealed.size(), password.data(), password.size(), &signer));
    char *json = nullptr; std::size_t json_length = 0;
    success(syrnck_signing_key_public_json(signer, &json, &json_length));
    const auto public_identity = parse_json(bytes(std::string_view(json, json_length - 1)));
    syrnck_string_free(json);
    require(public_identity["public_key"] == hex(SigningKey::from_seed(raw).public_key()), "opaque signer public identity differs");
    const auto signed_store = utf8(temporary.path / "signed-store");
    success(syrnck_store_open(signed_store.c_str(), true, nullptr, 0, &store.value));
    success(syrnck_store_use_native_signer(store.value, "Protected", "protected@example.invalid", 60, signer, true));
    success(syrnck_signing_key_close(signer)); signer = nullptr;
    const auto draft = Json{{"version", 1}, {"parents", Json::array()}, {"message", "opaque signer ABI"},
                            {"timestamp", 7}, {"files", Json::object()}}.dump();
    char signed_revision[65]{};
    success(syrnck_prepare_native_revision_ex(store.value, reinterpret_cast<const unsigned char *>(draft.data()), draft.size(), nullptr, signed_revision));
    success(syrnck_ref_set(store.value, "signed", "revision", signed_revision));
    success(syrnck_store_use_native_signer(store.value, "Required", "required@example.invalid", 0, nullptr, true));
    char blocked[65]{};
    require(syrnck_prepare_native_revision_ex(store.value, reinterpret_cast<const unsigned char *>(draft.data()), draft.size(), nullptr, blocked) != 0 && !blocked[0],
            "missing protected signer downgraded to unsigned");
    success(syrnck_store_close(store.value)); store.value = nullptr;
    {
      Store reopened(path_from_utf8(signed_store));
      const auto value = reopened.metadata(Kind::revision, signed_revision);
      require(value["signature"]["key_id"] == public_identity["key_id"], "closing protected signer invalidated Store-owned signing state");
      validate_object(Kind::revision, bytes(value.dump()));
    }
    Bytes changed(104, 0xa5);
    success(syrnck_signing_key_change_password(sealed.data(), sealed.size(), wrong.data(), wrong.size(), replacement.data(), replacement.size(), nullptr, nullptr, 0, &length));
    require(length == 104, "signing rewrap query authenticated or returned a different size");
    require(syrnck_signing_key_change_password(sealed.data(), sealed.size(), wrong.data(), wrong.size(), replacement.data(), replacement.size(), nullptr, changed.data(), changed.size(), &length) != 0 &&
                length == 0 && changed == untouched, "bad signing rewrap wrote output");
    success(syrnck_signing_key_change_password(sealed.data(), sealed.size(), password.data(), password.size(), replacement.data(), replacement.size(), nullptr, changed.data(), changed.size(), &length));
    success(syrnck_signing_key_open(changed.data(), changed.size(), replacement.data(), replacement.size(), &signer));
    success(syrnck_signing_key_public_json(signer, &json, &json_length));
    require(parse_json(bytes(std::string_view(json, json_length - 1))) == public_identity, "signing rewrap changed public owner identity");
    syrnck_string_free(json);
    success(syrnck_signing_key_close(signer)); signer = nullptr;
    json = reinterpret_cast<char *>(1); json_length = 9;
    require(syrnck_signing_key_public_json(nullptr, &json, &json_length) != 0 && !json && json_length == 0, "failed signer metadata outputs not reset");
    success(syrnck_signing_key_close(nullptr));
    std::cout << "Protected-key ABI ownership, capacity, queries, authentication and retained Store verified\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
