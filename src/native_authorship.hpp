#pragma once
#include "internal.hpp"
namespace syrnck {
Json canonical_native_author(const Json &, bool nullable);
Json canonical_native_signature(const Json &, const Json &unsigned_revision);
std::string native_revision_signing_message(const Json &unsigned_revision);
Json native_author_stamp(const NativeIdentity &, const Json &timestamp);
inline Json native_original_author(const Json &revision) {
  return revision.value("version", 1) == 2 ? revision.at("author") : Json{};
}
inline void append_native_authorship(Json &summary, const Json &revision) {
  if (revision.value("version", 1) != 2) return;
  summary["author"] = revision.at("author"); summary["committer"] = revision.at("committer");
  summary["signing_key"] = revision.at("signature").is_null() ? Json{} : revision.at("signature").at("key_id");
}
} // namespace syrnck
