#pragma once
#include "internal.hpp"
#include <syrnck/sealed.hpp>
#include <list>

namespace syrnck::sealed_detail {
inline constexpr std::string_view root_message = "Syrnck sealed snapshot v1";
inline constexpr std::string_view index_message = "Syrnck sealed index v1:";
inline constexpr std::string_view object_path = "sealed/objects/";
inline constexpr std::string_view root_path = "sealed/root";
inline constexpr std::size_t leaf_size = 256;
inline constexpr std::size_t frame_overhead = 64;

struct Budget {
  const SealedLimits &limits;
  OperationOptions options;
  std::size_t used = 0, index_reads = 0, index_writes = 0;
  void work(std::size_t amount = 1);
};

class Cipher {
public:
  explicit Cipher(const SecretKey &);
  std::string realm;
  std::string address(const ObjectKey &) const;
  Bytes seal(std::string_view domain, std::string_view name, std::span<const std::uint8_t>) const;
  Bytes open(std::string_view domain, std::string_view name, std::span<const std::uint8_t>,
             std::size_t maximum) const;

private:
  SecretKey addresses_, objects_, roots_;
};

using Entries = std::map<std::string, std::string>;
struct Node {
  std::string prefix;
  Entries entries;
  std::map<char, std::string> children;
};
class Index {
public:
  Index(const Store &, Budget &);
  std::optional<std::string> find(const std::string &root, const std::string &address);
  struct Change {
    std::string address, manifest;
  };
  std::string update(const Store &author, std::optional<std::string> root, const std::vector<Change> &);

private:
  const Store &wire_;
  Budget &budget_;
  std::list<std::string> recent_;
  struct Cached {
    std::shared_ptr<const Node> node;
    std::size_t bytes;
    std::list<std::string>::iterator at;
  };
  std::map<std::string, Cached> cache_;
  std::size_t bytes_ = 0;
  std::shared_ptr<const Node> read(const std::string &, const std::string &prefix);
  std::string rewrite(const Store &, const std::optional<std::string> &, const std::string &,
                      const std::vector<Change> &, std::size_t first, std::size_t last,
                      const Entries *seed = nullptr);
  std::string write(const Store &, const Node &);
};

struct Root {
  std::string id, index;
  std::optional<std::string> previous;
  Refs roots;
};
void channel_name(std::string_view);
Bytes read_file(const Store &, const std::string &, std::size_t maximum, Budget &);
Root read_root(const Store &, const Cipher &, std::string_view, std::string_view, Budget &);
std::optional<Root> read_checkpoint(const Store &, const Cipher &, std::string_view channel,
                                    std::string_view checkpoint, const Refs &);
bool descends_from(const Store &, const Cipher &, const Root &, std::string_view ancestor,
                   std::string_view channel, Budget &);
void distinct_stores(const Store &, const Store &);
void validate_wire_location(const Store &plaintext, const fs::path &wire, OperationOptions = {});
std::map<ObjectKey, bool> audit_source(const Store &, const Refs &, Budget &);
} // namespace syrnck::sealed_detail
