#include "native_history_fixture.hpp"
#include <syrnck.h>
#include <cstddef>
#include <iostream>
using namespace native_test;
namespace {
void success(int code) { require(code == 0, syrnck_last_error()); }
struct Handle {
  SyrnckStore *value = nullptr;
  ~Handle() { syrnck_store_close(value); }
  void close() {
    success(syrnck_store_close(value));
    value = nullptr;
  }
  void open(const fs::path &path, bool create, const std::optional<Key> &key = {}) {
    success(syrnck_store_open(utf8(path).c_str(), create, key ? key->data() : nullptr, key ? 32 : 0, &value));
  }
};
struct Master {
  SyrnckSealedKey *value = nullptr;
  ~Master() { syrnck_sealed_key_close(value); }
};
struct Cancellation {
  SyrnckCancellation *value = nullptr;
  Cancellation() { success(syrnck_cancellation_create(&value)); }
  ~Cancellation() { syrnck_cancellation_close(value); }
};
template <class Call> Json report(Call call) {
  char *out = nullptr;
  std::size_t length = 0;
  success(call(&out, &length));
  struct Free {
    char *p;
    ~Free() { syrnck_string_free(p); }
  } free{out};
  require(out && length > 1 && length <= 128 * 1024 * 1024 && out[length - 1] == 0,
          "invalid sealed ABI result length");
  return parse_unique_json({reinterpret_cast<const std::uint8_t *>(out), length - 1});
}
template <class Call> int refused(Call call) {
  char *out = reinterpret_cast<char *>(std::uintptr_t{1});
  std::size_t length = 999;
  const auto code = call(&out, &length);
  require(code != 0 && !out && !length, "failed sealed ABI call left outputs or succeeded");
  return code;
}
Json refs(Handle &store) {
  std::size_t size = 0;
  success(syrnck_refs_json(store.value, nullptr, 0, &size));
  Bytes data(size);
  success(syrnck_refs_json(store.value, reinterpret_cast<char *>(data.data()), data.size(), &size));
  return parse_unique_json(std::span(data).first(size - 1));
}
struct StopAt {
  SyrnckCancellation *source;
  std::uint32_t phase;
  unsigned occurrence = 1, seen = 0;
  bool requested = false, bad = false;
  static void progress(const SyrnckProgress *event, void *user) {
    auto &self = *static_cast<StopAt *>(user);
    if (!self.requested && event->phase == self.phase && event->complete && ++self.seen == self.occurrence) {
      self.requested = true;
      self.bad = syrnck_cancellation_request(self.source) != 0;
    }
  }
};
Json seal(Handle &source, Handle &wire, Master &key, const std::string &roots, const char *previous = nullptr,
          const SyrnckOperation *operation = nullptr) {
  return report([&](char **out, std::size_t *size) {
    return syrnck_sealed_snapshot_json_ex(source.value, wire.value, key.value, "team",
                                          reinterpret_cast<const unsigned char *>(roots.data()), roots.size(),
                                          previous, nullptr, operation, out, size);
  });
}
Json open(Handle &wire, Handle &target, Master &key, const std::string &id, const char *checkpoint,
          const SyrnckOperation *operation = nullptr) {
  return report([&](char **out, std::size_t *size) {
    return syrnck_sealed_open_json_ex(wire.value, target.value, key.value, id.c_str(), "team", checkpoint,
                                      nullptr, operation, out, size);
  });
}
std::pair<std::string, std::string> revision(Handle &source, const std::string &payload,
                                             const char *parent = nullptr) {
  const SyrnckChunking chunks{1, 1024, 1024, 1024};
  char manifest[65]{}, id[65]{};
  success(syrnck_ingest_buffer_ex(source.value, reinterpret_cast<const unsigned char *>(payload.data()),
                                  payload.size(), &chunks, nullptr, manifest));
  const auto value =
      Json{{"version", 1},
           {"parents", parent ? Json::array({parent}) : Json::array()},
           {"message", "PRIVATE-SOURCE-MESSAGE"},
           {"timestamp", 1},
           {"files", Json{{"Content/Private.uasset", Json{{"manifest", manifest}, {"executable", false}}}}}}
          .dump();
  success(syrnck_prepare_native_revision_ex(
      source.value, reinterpret_cast<const unsigned char *>(value.data()), value.size(), nullptr, id));
  success(syrnck_ref_set(source.value, "heads/private", "revision", id));
  return {manifest, id};
}
void scenario(const fs::path &path, Master &key, bool encrypted, unsigned layout) {
  std::optional<Key> at_rest;
  if (encrypted) {
    at_rest.emplace();
    at_rest->fill(0x31);
  }
  Handle source, wire, target;
  source.open(path / "source", true, at_rest);
  if (at_rest)
    at_rest->fill(0x32);
  wire.open(path / "wire", true, at_rest);
  if (at_rest)
    at_rest->fill(0x33);
  target.open(path / "target", true, at_rest);
  const auto original = std::string(1024, 'a') + std::string(1024, 'b') + std::string(1024, 'c');
  const auto first = revision(source, original);
  const auto roots = encode_refs({{"heads/private", {Kind::revision, first.second}}}).dump();
  const auto unchanged_source = refs(source), untouched_target = refs(target);
  if (!encrypted && !layout) {
    const auto typed = key_json({Kind::revision, first.second}).dump();
    for (const auto &invalid :
         {std::string("{}"), "{\"same\":" + typed + ",\"same\":" + typed + '}',
          "{\"file\":{\"kind\":\"revision\",\"kind\":\"revision\",\"id\":\"" + first.second + "\"}}"}) {
      refused([&](char **out, std::size_t *size) {
        return syrnck_sealed_snapshot_json_ex(source.value, wire.value, key.value, "team",
                                              reinterpret_cast<const unsigned char *>(invalid.data()),
                                              invalid.size(), nullptr, nullptr, nullptr, out, size);
      });
    }
    for (const auto length : {std::size_t{0}, max_object_size + 1})
      refused([&](char **out, std::size_t *size) {
        return syrnck_sealed_snapshot_json_ex(source.value, wire.value, key.value, "team", nullptr, length,
                                              nullptr, nullptr, nullptr, out, size);
      });
    require(refs(wire).empty() && refs(source) == unchanged_source, "invalid sealed ABI roots changed refs");
  }
  {
    Cancellation cancellation;
    StopAt stop{cancellation.value, SYRNCK_PHASE_INGESTING};
    const SyrnckOperation operation{cancellation.value, StopAt::progress, &stop};
    require(refused([&](char **out, std::size_t *size) {
              return syrnck_sealed_snapshot_json_ex(source.value, wire.value, key.value, "team",
                                                    reinterpret_cast<const unsigned char *>(roots.data()),
                                                    roots.size(), nullptr, nullptr, &operation, out, size);
            }) == SYRNCK_CANCELLED &&
                stop.requested && !stop.bad,
            "sealed cancellation did not cross the C ABI");
    require(refs(wire).empty() && refs(source) == unchanged_source,
            "cancelled ABI preparation published refs");
  }
  Json prepared;
  {
    Cancellation cancellation;
    StopAt stop{cancellation.value, SYRNCK_PHASE_PLANNING};
    const SyrnckOperation operation{cancellation.value, StopAt::progress, &stop};
    prepared = seal(source, wire, key, roots, nullptr, &operation);
    require(stop.requested && !stop.bad, "late sealed cancellation checkpoint was not reached");
  }
  const auto snapshot = prepared["snapshot"].get<std::string>();
  require(refs(source) == unchanged_source && refs(wire).size() == 1,
          "sealed ABI preparation changed source or lost wire retention");
  if (layout) {
    wire.close();
    auto wire_key = at_rest;
    if (wire_key)
      wire_key->fill(0x32);
    {
      Store packed(path / "wire", wire_key);
      packed.pack();
      if (layout == 2)
        packed.pack({.compact = true});
    }
    wire.open(path / "wire", false, wire_key);
  }
  const auto before_inspect = refs(wire);
  const auto inspected = report([&](char **out, std::size_t *size) {
    return syrnck_sealed_inspect_json_ex(wire.value, key.value, snapshot.c_str(), "team", nullptr, nullptr,
                                         out, size);
  });
  require(inspected["roots"].dump() == roots && refs(wire) == before_inspect,
          "sealed ABI inspection lost roots or changed refs");
  {
    Cancellation cancellation;
    StopAt stop{cancellation.value, SYRNCK_PHASE_RECONSTRUCTING, 3};
    const SyrnckOperation operation{cancellation.value, StopAt::progress, &stop};
    require(refused([&](char **out, std::size_t *size) {
              return syrnck_sealed_open_json_ex(wire.value, target.value, key.value, snapshot.c_str(), "team",
                                                "sealed/checkpoints/team", nullptr, &operation, out, size);
            }) == SYRNCK_CANCELLED &&
                stop.requested && !stop.bad,
            "sealed import cancellation did not cross the C ABI");
    require(refs(target) == untouched_target, "cancelled sealed import published a checkpoint");
  }
  {
    Cancellation cancellation;
    StopAt stop{cancellation.value, SYRNCK_PHASE_PLANNING};
    const SyrnckOperation operation{cancellation.value, StopAt::progress, &stop};
    const auto opened = open(wire, target, key, snapshot, "sealed/checkpoints/team", &operation);
    require(stop.requested && !stop.bad && opened["roots"] == inspected["roots"],
            "published sealed import lost late cancellation result");
  }
  auto payload = original;
  payload.replace(1024, 1024, std::string(1024, 'd'));
  const auto second = revision(source, payload, first.second.c_str());
  const auto updated_roots = encode_refs({{"heads/private", {Kind::revision, second.second}}}).dump();
  const auto updated = seal(source, wire, key, updated_roots, snapshot.c_str());
  require(updated["sealed_objects"] == 3 && updated["reused_objects"] == prepared["objects"],
          "sealed ABI update rewrote unchanged original blocks");
  open(wire, target, key, updated["snapshot"].get<std::string>(), "sealed/checkpoints/team");
  const auto checkpoint = refs(target);
  refused([&](char **out, std::size_t *size) {
    return syrnck_sealed_open_json_ex(wire.value, target.value, key.value, snapshot.c_str(), "team",
                                      "sealed/checkpoints/team", nullptr, nullptr, out, size);
  });
  require(refs(target) == checkpoint && !checkpoint.contains("heads/private"),
          "ABI rollback changed checkpoint or native heads");
  open(wire, target, key, snapshot, "sealed/checkpoints/history");
  for (const auto &[manifest, expected] :
       {std::pair{first.first, original}, std::pair{second.first, payload}}) {
    Bytes out(expected.size());
    std::size_t size = 0;
    success(
        syrnck_file_read_range(target.value, manifest.c_str(), 0, out.size(), out.data(), out.size(), &size));
    require(size == out.size() && text(out) == expected, "sealed ABI historical reconstruction lost bytes");
  }
  const SyrnckSealedLimits invalid{UINT64_MAX, 0, 0, 0, 0};
  refused([&](char **out, std::size_t *size) {
    return syrnck_sealed_inspect_json_ex(wire.value, key.value, snapshot.c_str(), "team", &invalid, nullptr,
                                         out, size);
  });
  std::size_t count = 0;
  success(syrnck_store_verify(target.value, &count));
  require(count > 0, "sealed ABI retained no originals");
}
} // namespace
int main() {
  Temporary temporary;
  try {
    static_assert(sizeof(SyrnckSealedLimits) == 40 && offsetof(SyrnckSealedLimits, max_history) == 32);
    const Bytes password{'p', 0, 'a'}, wrong{'b'}, next{'n', 0, 'b'};
    Key raw;
    raw.fill(0x71);
    Bytes envelope(104, 0xa5), sentinel = envelope;
    std::size_t length = 0;
    success(syrnck_sealed_key_protect(raw.data(), raw.size(), password.data(), password.size(), nullptr,
                                      nullptr, 0, &length));
    require(length == 104, "invalid sealed key query");
    require(syrnck_sealed_key_protect(raw.data(), raw.size(), password.data(), password.size(), nullptr,
                                      envelope.data(), 103, &length) != 0 &&
                length == 104 && envelope == sentinel,
            "short sealed envelope output changed bytes");
    success(syrnck_sealed_key_protect(raw.data(), raw.size(), password.data(), password.size(), nullptr,
                                      envelope.data(), envelope.size(), &length));
    Master master;
    require(syrnck_sealed_key_open(envelope.data(), envelope.size(), wrong.data(), wrong.size(),
                                   &master.value) != 0 &&
                !master.value,
            "wrong sealed password returned an owner");
    success(syrnck_sealed_key_open(envelope.data(), envelope.size(), password.data(), password.size(),
                                   &master.value));
    const auto metadata = report([&](char **out, std::size_t *size) {
      return syrnck_sealed_key_public_json(master.value, out, size);
    });
    require(metadata.size() == 3 && metadata["realm"] == "d9ec988a39dfbc52de95293fb84f3f03",
            "sealed ABI metadata leaked a key or changed realm");
    SyrnckSecretKey *storage = nullptr;
    SyrnckSigningKey *signer = nullptr;
    require(syrnck_secret_key_open(envelope.data(), envelope.size(), password.data(), password.size(),
                                   &storage) != 0 &&
                !storage &&
                syrnck_signing_key_open(envelope.data(), envelope.size(), password.data(), password.size(),
                                        &signer) != 0 &&
                !signer,
            "sealed key opened with another purpose");
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout)
        scenario(temporary.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), master,
                 encrypted, layout);
    Bytes wrapped(104);
    success(syrnck_sealed_key_change_password(envelope.data(), envelope.size(), password.data(),
                                              password.size(), next.data(), next.size(), nullptr,
                                              wrapped.data(), wrapped.size(), &length));
    Master rewrapped;
    success(
        syrnck_sealed_key_open(wrapped.data(), wrapped.size(), next.data(), next.size(), &rewrapped.value));
    require(report([&](char **out, std::size_t *size) {
              return syrnck_sealed_key_public_json(rewrapped.value, out, size);
            }) == metadata,
            "password change rotated sealed master");
    refused([&](char **out, std::size_t *size) { return syrnck_sealed_key_public_json(nullptr, out, size); });
    success(syrnck_sealed_key_close(nullptr));
    std::cout << "Sealed C ABI ownership, cancellation, differential snapshots, rollback and retained "
                 "history verified\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
