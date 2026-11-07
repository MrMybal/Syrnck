#include "cli_help.hpp"
#include "internal.hpp"
#include <cctype>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
namespace syrnck::cli {
namespace {
std::string encode(std::string_view value, bool path = false) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char c : value)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~' || (path && c == '/'))
      result += char(c);
    else {
      result += '%';
      result += digits[c >> 4];
      result += digits[c & 15];
    }
  return result;
}
std::string decode(std::string_view value) {
  std::string result;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      result += value[i];
      continue;
    }
    require(i + 2 < value.size(), "invalid documentation address");
    auto digit = [](unsigned char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    const auto high = digit(value[i + 1]), low = digit(value[i + 2]);
    require(high >= 0 && low >= 0, "invalid documentation address");
    result += char(high * 16 + low);
    i += 2;
  }
  return result;
}
std::string file_uri(const fs::path &path) {
  const auto absolute = fs::canonical(path);
#ifdef _WIN32
  // Escape the native UTF-16 path as UTF-8, including literal '%' and '#'.
  // UrlCreateFromPathW uses legacy byte escapes for some non-ASCII names.
  // https://learn.microsoft.com/windows/win32/api/shlwapi/nf-shlwapi-urlescapew
  const auto native = absolute.generic_wstring();
  std::wstring uri(32768, L'\0');
  DWORD size = static_cast<DWORD>(uri.size());
  require(SUCCEEDED(UrlEscapeW(native.c_str(), uri.data(), &size,
                               URL_ESCAPE_AS_UTF8 | URL_ESCAPE_SEGMENT_ONLY | URL_ESCAPE_PERCENT)),
          "cannot create documentation file URL");
  uri.resize(size);
  auto value = utf8(fs::path(uri));
  // Only separators introduced by the path itself are restored. An original
  // percent-encoded-looking filename is protected by URL_ESCAPE_PERCENT.
  for (std::size_t at = 0; (at = value.find("%2F", at)) != value.npos; ++at) {
    value.replace(at, 3, "/");
  }
  return (value.starts_with("//") ? "file:" : "file:///") + value;
#else
  return "file://" + encode(utf8(absolute), true);
#endif
}
} // namespace
fs::path documentation_directory(const std::optional<std::string> &override_directory) {
  if (override_directory)
    return fs::absolute(path_from_utf8(*override_directory));
  return executable_path().parent_path() / "Documentation";
}
std::string documentation_url(const fs::path &directory, Language language, std::string topic,
                              const std::optional<std::string> &section) {
  const auto reader = directory / "Documentation.html";
  require(fs::is_regular_file(reader), "local documentation is not installed; rebuild or select --directory");
  const auto manifest = parse_json(read_bytes(directory / "DocData/manifest.json", max_object_size));
  const auto topics = parse_json(read_bytes(directory / "DocData/help-topics.json", max_object_size));
  require(manifest.is_object() && manifest.value("schemaVersion", 0) == 1 && manifest.contains("pages") &&
              manifest["pages"].is_array() && topics.is_object(),
          "invalid documentation catalog");
  topic = decode(topic);
  std::replace(topic.begin(), topic.end(), '\\', '/');
  if (topic.starts_with("./"))
    topic.erase(0, 2);
  if (topic.empty())
    topic = "overview";
  if (section) {
    require(topic.find('#') == topic.npos, "documentation section specified twice");
    topic += '#' + decode(*section);
  }
  auto selected = topics.find(topic);
  auto target = selected != topics.end() ? selected->get<std::string>() : topic;
  auto delimiter = target.find('#');
  auto id = target.substr(0, delimiter);
  std::string anchor = delimiter == target.npos ? "" : target.substr(delimiter + 1);
  if (auto found = topics.find(id); found != topics.end()) {
    const auto mapped = found->get<std::string>();
    const auto split = mapped.find('#');
    id = mapped.substr(0, split);
    if (anchor.empty() && split != mapped.npos)
      anchor = mapped.substr(split + 1);
  }
  const auto locale = language == Language::french ? "fr" : "en";
  const Json *page = nullptr;
  for (const auto &entry : manifest["pages"])
    if (entry.value("id", "") == id && entry.value("locale", "") == locale) {
      page = &entry;
      break;
    }
  require(page, "unknown documentation topic");
  if (!anchor.empty()) {
    bool present = false;
    for (const auto &heading : page->at("headings"))
      present |= heading.at("id") == anchor;
    for (const auto &extra : page->value("anchors", Json::array()))
      present |= extra == anchor;
    require(present, "unknown documentation section");
  }
  return file_uri(reader) + "#/" + locale + '/' + encode(id) + (anchor.empty() ? "" : '/' + encode(anchor));
}
void open_documentation(std::string_view url) {
#ifdef _WIN32
  const auto value = path_from_utf8(url).wstring();
  const auto result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", value.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  require(result > 32, "cannot open documentation; use --print-url");
#else
  std::string value(url);
#ifdef __APPLE__
  const char *program = "open";
#else
  const char *program = "xdg-open";
#endif
  char *argv[] = {const_cast<char *>(program), value.data(), nullptr};
  pid_t process;
  require(posix_spawnp(&process, program, nullptr, nullptr, argv, environ) == 0,
          "cannot open documentation; use --print-url");
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(process, &status, 0);
  } while (waited < 0 && errno == EINTR);
  require(waited == process && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "cannot open documentation; use --print-url");
#endif
}
} // namespace syrnck::cli
