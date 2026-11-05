#include "native_history_fixture.hpp"
#include "native_text_merge.hpp"
#include <iostream>
#include <random>
using namespace native_test;
namespace {
NativeSnapshotFile entry(const Fixture &f, std::string_view content, bool mode = false) {
  return {f.file(content, mode)["manifest"].get<std::string>(), {content.size(), hash(bytes(content))}, mode};
}
void run(const fs::path &root, std::optional<Key> key, unsigned layout) {
  Fixture f(root, key);
  const auto base = entry(f, "a\r\nb\r\nc"), ours = entry(f, "A\r\nb\r\nc", true),
             theirs = entry(f, "a\r\nb\r\nC"), overlap = entry(f, "B\r\nb\r\nc");
  f.pack(layout);
  const auto before = f.physical();
  NativeTextMergeBatch batch(f.store, {});
  const auto merged = batch.merge("code.cpp", base, ours, theirs, {});
  require(merged && merged->executable && batch.generated.size() == 1 &&
              batch.generated.at(merged->manifest) == "A\r\nb\r\nC" &&
              !f.store.contains({Kind::manifest, merged->manifest}),
          "batch wrote or lost merged text");
  require(batch.merge("copy.cpp", base, ours, theirs, {})->manifest == merged->manifest &&
              batch.generated.size() == 1,
          "batch did not deduplicate candidates");
  require(!batch.merge("conflict.cpp", base, ours, overlap, {}) &&
              batch.report()["files"][2]["status"] == "conflict",
          "batch hid overlap");
  require(f.physical() == before, "batch planning changed the Store");
  materialize_native_text(f.store, batch.generated, {});
  std::ostringstream output;
  f.store.reconstruct(merged->manifest, output);
  require(output.str() == "A\r\nb\r\nC", "materialized content or predicted ID differs");
  NativeTextMergeBatch count(f.store, {});
  for (unsigned i = 0; i <= NativeTextMergeBatch::max_files; ++i)
    count.merge(std::to_string(i), base, ours, overlap, {});
  require(count.report()["attempted_files"] == NativeTextMergeBatch::max_files &&
              count.report()["files"].back()["reason"] == "file_count_limit",
          "batch count is unbounded");
  TextDiffOptions limits;
  limits.max_file_bytes = 1;
  NativeTextMergeBatch small(f.store, limits);
  require(!small.merge("large", base, ours, theirs, {}) && small.report()["input_bytes"] == 0 &&
              small.report()["files"][0]["reason"] == "file_limit",
          "oversized batch read payloads");
  std::stop_source stop;
  stop.request_stop();
  fails([&] { batch.merge("cancel", base, ours, theirs, {stop.get_token()}); });
}
void prediction(const fs::path &root) {
  Fixture f(root);
  std::mt19937 random(0x537972);
  for (const auto size : {0U, 1U, 262144U, 1048576U, 4194304U}) {
    Bytes data(size);
    for (auto &value : data)
      value = static_cast<std::uint8_t>(random());
    const auto content = text(data);
    const auto predicted = native_text_manifest(content);
    require(!f.store.contains({Kind::manifest, predicted}), "manifest prediction wrote an object");
    materialize_native_text(f.store, {{predicted, content}}, {});
    require(f.store.contains({Kind::manifest, predicted}), "CDC prediction differs from ingestion");
  }
  // Inputs plus clean results consume the aggregate budget; the next candidate
  // must be classified without opening its missing manifest.
  std::string large(4 * 1024 * 1024, 'x');
  auto present = entry(f, large);
  const NativeSnapshotFile empty{entry(f, "")};
  TextDiffOptions limits;
  limits.max_file_bytes = 4 * 1024 * 1024;
  limits.max_work = 256 * 1024 * 1024;
  limits.max_output_bytes = 8 * 1024 * 1024;
  NativeTextMergeBatch bytes_limit(f.store, limits);
  // Each result charges another 4 MiB, so two candidates consume 16 MiB.
  require(bytes_limit.merge("first", empty, present, empty, {}).has_value(), "first aggregate merge failed");
  require(bytes_limit.merge("second", empty, present, empty, {}).has_value(),
          "second aggregate merge failed");
  present.manifest = std::string(64, '0');
  require(!bytes_limit.merge("third", empty, present, empty, {}) &&
              bytes_limit.report()["files"].back()["reason"] == "total_bytes_limit",
          "aggregate bytes unbounded");
}
} // namespace
int main() {
  Temporary tmp;
  try {
    for (bool encrypted : {false, true})
      for (unsigned layout = 0; layout < 3; ++layout) {
        std::optional<Key> key;
        if (encrypted) {
          key.emplace();
          key->fill(91);
        }
        run(tmp.path / (std::to_string(encrypted) + '-' + std::to_string(layout)), key, layout);
      }
    prediction(tmp.path / "prediction");
    std::cout << "Read-only bounded text batches, exact CDC candidate IDs and verified materialization\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
