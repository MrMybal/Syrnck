#include "internal.hpp"
#include <syrnck/git.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <streambuf>

namespace syrnck::git {
namespace {
constexpr std::size_t packet_payload = 65516, metadata_limit = 65536, hold_limit = 65536;
struct Packet {
  bool flush = false;
  std::string data;
};
std::optional<Packet> read_packet(std::istream &input, std::stop_token stop, bool allow_eof = false) {
  throw_if_cancelled(stop);
  std::array<char, 4> header{};
  input.read(header.data(), 4);
  if (allow_eof && input.gcount() == 0 && input.eof() && !input.bad())
    return {};
  require(input.gcount() == 4 && !input.bad(), "truncated Git filter packet");
  std::size_t size = 0;
  for (char c : header) {
    require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "invalid Git filter packet length");
    size = size * 16 + static_cast<std::size_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  if (size == 0)
    return Packet{true, {}};
  require(size >= 4 && size <= packet_payload + 4, "invalid Git filter packet length");
  Packet packet{false, std::string(size - 4, '\0')};
  input.read(packet.data.data(), static_cast<std::streamsize>(packet.data.size()));
  require(input.gcount() == static_cast<std::streamsize>(packet.data.size()) && !input.bad(),
          "truncated Git filter packet");
  throw_if_cancelled(stop);
  return packet;
}
void packet(std::ostream &output, std::string_view data) {
  require(data.size() <= packet_payload, "Git filter output packet exceeds size limit");
  constexpr char hex[] = "0123456789abcdef";
  auto size = data.size() + 4;
  std::array<char, 4> header{};
  for (auto at = header.rbegin(); at != header.rend(); ++at) {
    *at = hex[size & 15];
    size >>= 4;
  }
  output.write(header.data(), 4);
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  require(output.good(), "Git filter protocol write failed");
}
void flush(std::ostream &output) {
  output.write("0000", 4);
  require(output.good(), "Git filter protocol write failed");
}
void sync_protocol(std::ostream &output) {
  output.flush();
  require(output.good(), "Git filter protocol write failed");
}
std::optional<std::vector<std::string>> read_list(std::istream &input, std::stop_token stop,
                                                  bool allow_eof = false) {
  std::vector<std::string> lines;
  std::size_t total = 0;
  for (;;) {
    auto next = read_packet(input, stop, allow_eof && lines.empty());
    if (!next) {
      require(lines.empty(), "truncated Git filter metadata");
      return {};
    }
    if (next->flush)
      return lines;
    require(next->data.size() <= metadata_limit - total && lines.size() < 256,
            "Git filter metadata exceeds size limit");
    total += next->data.size();
    if (next->data.ends_with('\n'))
      next->data.pop_back();
    require(next->data.find('\0') == next->data.npos, "invalid Git filter metadata");
    lines.push_back(std::move(next->data));
  }
}
class Body : public std::streambuf {
  std::istream &input_;
  std::stop_token stop_;
  std::string data_;
  std::exception_ptr failure_;
  bool finished_ = false;

public:
  Body(std::istream &input, std::stop_token stop) : input_(input), stop_(stop) {}
  bool finished() const {
    return finished_;
  }
  void check() const {
    if (failure_)
      std::rethrow_exception(failure_);
  }
  void discard() {
    check();
    setg(nullptr, nullptr, nullptr);
    while (!finished_) {
      auto next = read_packet(input_, stop_);
      finished_ = next->flush;
    }
  }
  int_type underflow() override {
    try {
      check();
      while (!finished_) {
        auto next = read_packet(input_, stop_);
        finished_ = next->flush;
        if (finished_)
          break;
        data_ = std::move(next->data);
        if (!data_.empty()) {
          setg(data_.data(), data_.data(), data_.data() + data_.size());
          return traits_type::to_int_type(*gptr());
        }
      }
      return traits_type::eof();
    } catch (...) {
      failure_ = std::current_exception();
      throw;
    }
  }
};
class Content : public std::streambuf {
  struct CloseSpool {
    void operator()(std::FILE *file) const noexcept {
      if (file)
        std::fclose(file);
    }
  };
  std::ostream &output_;
  const Body &body_;
  std::stop_token stop_;
  std::string held_;
  std::unique_ptr<std::FILE, CloseSpool> spool_;
  bool started_ = false;

  void emit(std::string_view data) {
    while (!data.empty()) {
      throw_if_cancelled(stop_);
      auto count = std::min(packet_payload, data.size());
      packet(output_, data.substr(0, count));
      data.remove_prefix(count);
    }
  }
  void begin() {
    if (started_)
      return;
    started_ = true;
    packet(output_, "status=success\n");
    flush(output_);
    emit(held_);
    held_.clear();
  }
  void write(std::string_view data) {
    throw_if_cancelled(stop_);
    if (data.empty())
      return;
    if (!spool_ && body_.finished()) {
      begin();
      emit(data);
    } else if (!spool_ && data.size() <= hold_limit - held_.size()) {
      held_ += data;
    } else {
      if (!spool_) {
        spool_.reset(std::tmpfile());
        require(bool(spool_), "cannot allocate Git filter temporary stream");
        require(std::fwrite(held_.data(), 1, held_.size(), spool_.get()) == held_.size(),
                "Git filter temporary stream write failed");
        held_.clear();
      }
      require(std::fwrite(data.data(), 1, data.size(), spool_.get()) == data.size(),
              "Git filter temporary stream write failed");
    }
  }

public:
  Content(std::ostream &output, const Body &body, std::stop_token stop)
      : output_(output), body_(body), stop_(stop) {}
  std::streamsize xsputn(const char *data, std::streamsize size) override {
    write({data, static_cast<std::size_t>(size)});
    return size;
  }
  int_type overflow(int_type c) override {
    if (!traits_type::eq_int_type(c, traits_type::eof())) {
      char value = traits_type::to_char_type(c);
      write({&value, 1});
    }
    return traits_type::not_eof(c);
  }
  void finish(bool success) {
    require(body_.finished(), "Git filter response precedes input completion");
    throw_if_cancelled(stop_);
    if (!started_ && !success) {
      packet(output_, "status=error\n");
      flush(output_);
      sync_protocol(output_);
      return;
    }
    begin();
    if (spool_ && success) {
      require(std::fflush(spool_.get()) == 0 && std::fseek(spool_.get(), 0, SEEK_SET) == 0,
              "Git filter temporary stream rewind failed");
      std::array<char, packet_payload> buffer{};
      for (;;) {
        throw_if_cancelled(stop_);
        auto count = std::fread(buffer.data(), 1, buffer.size(), spool_.get());
        emit({buffer.data(), count});
        if (count < buffer.size()) {
          require(!std::ferror(spool_.get()), "Git filter temporary stream read failed");
          break;
        }
      }
    }
    flush(output_);
    if (!success)
      packet(output_, "status=error\n");
    flush(output_);
    sync_protocol(output_);
  }
};
} // namespace

void filter_process(std::istream &input, std::ostream &output, const FilterHandler &handler,
                    OperationOptions operation, const std::function<void(std::string_view)> &diagnostic) {
  require(bool(handler), "missing Git filter handler");
  auto hello = read_list(input, operation.stop);
  require(hello && hello->size() >= 2 && hello->front() == "git-filter-client" &&
              std::find(hello->begin() + 1, hello->end(), "version=2") != hello->end(),
          "unsupported Git filter handshake");
  for (auto at = hello->begin() + 1; at != hello->end(); ++at)
    require(at->starts_with("version=") && at->size() > 8 &&
                std::all_of(at->begin() + 8, at->end(), [](char c) { return c >= '0' && c <= '9'; }),
            "invalid Git filter handshake");
  packet(output, "git-filter-server\n");
  packet(output, "version=2\n");
  flush(output);
  sync_protocol(output);
  auto offered = read_list(input, operation.stop);
  std::set<std::string> capabilities;
  for (const auto &line : *offered) {
    require(line.starts_with("capability=") && line.size() > 11, "invalid Git filter capabilities");
    if (line == "capability=clean" || line == "capability=smudge")
      capabilities.insert(line.substr(11));
  }
  require(!capabilities.empty(), "no supported Git filter capability");
  for (const auto &capability : capabilities)
    packet(output, "capability=" + capability + "\n");
  flush(output);
  sync_protocol(output);
  while (auto lines = read_list(input, operation.stop, true)) {
    Body body(input, operation.stop);
    std::istream source(&body);
    Content content(output, body, operation.stop);
    std::ostream destination(&content);
    std::string error;
    try {
      std::map<std::string, std::string> fields;
      for (const auto &line : *lines) {
        auto equal = line.find('=');
        require(equal != line.npos && equal > 0 &&
                    fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second,
                "invalid Git filter request headers");
      }
      auto command = fields.find("command"), path = fields.find("pathname");
      require(command != fields.end() && capabilities.contains(command->second) && path != fields.end() &&
                  !path->second.empty(),
              "invalid Git filter command or pathname");
      handler(command->second, path->second, source, destination);
      require(destination.good(), "Git filter content write failed");
    } catch (const Cancelled &) {
      throw;
    } catch (const std::exception &failure) {
      error = failure.what();
      // Empty what() strings must still produce an error response.
      if (error.empty())
        error = "Git filter operation failed";
    }
    body.discard(); // A file error must not desynchronize subsequent requests.
    if (!error.empty() && diagnostic)
      diagnostic(error);
    content.finish(error.empty());
  }
}
} // namespace syrnck::git
