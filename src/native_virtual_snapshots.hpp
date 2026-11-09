#pragma once
#include "native_workspace.hpp"
namespace syrnck {
// Retain candidate changes rather than one complete asset map per revision.
// A single recent full map accelerates linear stretches; branch snapshots are
// rebuilt iteratively from a real ancestor and bounded in-memory deltas.
class NativeVirtualSnapshots {
public:
  NativeVirtualSnapshots(const Store &, std::size_t max_delta_bytes, OperationOptions,
                         std::function<void(std::size_t)> work = {});
  NativeSnapshot read(std::string_view id);
  void add(std::string id, std::optional<std::string> parent, const NativeSnapshot &after);
  bool contains(std::string_view id) const { return records_.contains(std::string(id)); }
  Json report() const;

private:
  struct Delta {
    std::optional<std::string> parent;
    std::map<std::string, std::optional<NativeSnapshotFile>> files;
  };
  const Store &store_;
  const std::size_t limit_;
  OperationOptions options_;
  std::function<void(std::size_t)> work_;
  std::map<std::string, Delta> records_;
  std::size_t bytes_ = 0, entries_ = 0;
  std::string recent_id_;
  NativeSnapshot recent_;
  void work(std::size_t);
};
} // namespace syrnck
