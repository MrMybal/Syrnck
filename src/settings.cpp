#include "internal.hpp"
#include "pack_storage.hpp"
namespace syrnck {
void RepositorySettings::validate() const {
  require(ingest_workers >= 1 && ingest_workers <= 64, "ingest workers must be in 1..64");
  require(ingest_buffer_bytes >= 1024 * 1024 && ingest_buffer_bytes <= 1024ULL * 1024 * 1024,
          "buffer budget must be in 1..1024 MiB");
  PackStorage::validate_options(pack_options());
}
IngestOptions RepositorySettings::ingest_options() const {
  return {.workers = ingest_workers, .max_in_flight_bytes = ingest_buffer_bytes};
}
PackOptions RepositorySettings::pack_options() const {
  return {.max_pack_bytes = pack_max_bytes, .max_objects_per_pack = pack_max_objects};
}
Json RepositorySettings::json() const {
  validate();
  return Json{{"version", 1},
              {"ingest", Json{{"workers", ingest_workers}, {"buffer_bytes", ingest_buffer_bytes}}},
              {"pack", Json{{"max_bytes", pack_max_bytes}, {"max_objects", pack_max_objects}}}};
}
RepositorySettings RepositorySettings::parse(const Json &value) {
  require(value.is_object() && value.size() == 3 && value.contains("version") &&
              value["version"].is_number_integer() && value["version"] == 1 && value.contains("ingest") &&
              value.contains("pack"),
          "invalid repository settings");
  const auto &ingest = value["ingest"], &pack = value["pack"];
  require(ingest.is_object() && ingest.size() == 2 && pack.is_object() && pack.size() == 2 &&
              ingest.contains("workers") && ingest["workers"].is_number_unsigned() &&
              ingest.contains("buffer_bytes") && ingest["buffer_bytes"].is_number_unsigned() &&
              pack.contains("max_bytes") && pack["max_bytes"].is_number_unsigned() &&
              pack.contains("max_objects") && pack["max_objects"].is_number_unsigned(),
          "invalid repository settings");
  auto workers = ingest["workers"].get<std::uint64_t>();
  auto buffer = ingest["buffer_bytes"].get<std::uint64_t>();
  auto objects = pack["max_objects"].get<std::uint64_t>();
  require(workers >= 1 && workers <= 64 && buffer >= 1024 * 1024 && buffer <= 1024ULL * 1024 * 1024 &&
              objects >= 1 && objects <= PackStorage::max_entries,
          "invalid repository settings");
  RepositorySettings result{static_cast<std::size_t>(workers), static_cast<std::size_t>(buffer),
                            pack["max_bytes"].get<std::uint64_t>(), static_cast<std::size_t>(objects)};
  result.validate();
  return result;
}
RepositorySettings Store::settings() const {
  auto path = root() / "settings.json";
  auto status = fs::symlink_status(path);
  if (status.type() == fs::file_type::not_found)
    return {};
  require(fs::is_regular_file(status) && !fs::is_symlink(status), "invalid repository settings file");
  return RepositorySettings::parse(parse_json(read_bytes(path, 4096)));
}
void Store::set_settings(const RepositorySettings &value) const {
  value.validate();
  auto path = root() / "settings.json";
  auto status = fs::symlink_status(path);
  require(status.type() == fs::file_type::not_found ||
              (fs::is_regular_file(status) && !fs::is_symlink(status)),
          "invalid repository settings file");
  atomic_write(path, bytes(value.json().dump()), status.type() != fs::file_type::not_found);
}
std::pair<std::string, IngestStats> Store::ingest(std::istream &source, Chunking chunking) const {
  return ingest(source, chunking, settings().ingest_options());
}
Json Store::pack() const {
  return pack(settings().pack_options());
}
} // namespace syrnck
