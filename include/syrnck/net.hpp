#pragma once
#include <syrnck/core.hpp>
#include <syrnck/i18n.hpp>
namespace syrnck::net {
void serve(Store &, std::string_view token, std::string_view listen = "127.0.0.1:7437",
           Language language = Language::english);
class Remote {
public:
  Remote(std::string url, std::string token, std::optional<fs::path> ca = {});
  Refs refs() const;
  Json push(const Store &, const ObjectKey &, std::string_view remote_ref) const;
  Json pull(const Store &, const ObjectKey &, std::string_view local_ref) const;

private:
  std::string base_, token_;
  std::optional<fs::path> ca_;
  Bytes request(std::string_view method, std::string_view path,
                std::span<const std::uint8_t> body = {}) const;
};
} // namespace syrnck::net
