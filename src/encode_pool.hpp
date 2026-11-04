#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <syrnck/core.hpp>
#include <zstd.h>

namespace syrnck {
// Internal CPU-only workers. The ingest coordinator bounds submitted payloads
// and performs every store write, keeping refs and object publication serialized.
class EncodePool {
public:
  using Encoder = std::function<Bytes(ZSTD_CCtx *, const ObjectKey &, std::span<const std::uint8_t>)>;
  EncodePool(std::size_t count, Encoder encoder) : encoder_(std::move(encoder)) {
    for (std::size_t i = 0; i < count; ++i)
      workers_.emplace_back([this](std::stop_token stop) { run(stop); });
  }
  ~EncodePool() {
    for (auto &worker : workers_)
      worker.request_stop();
    ready_.notify_all();
    workers_.clear(); // Join before destroying the queue, codec callback or store.
  }
  EncodePool(const EncodePool &) = delete;
  EncodePool &operator=(const EncodePool &) = delete;

  std::future<Bytes> submit(ObjectKey key, Bytes bytes) {
    Job job{std::move(key), std::move(bytes), {}};
    auto result = job.result.get_future();
    {
      std::lock_guard lock(mutex_);
      jobs_.push_back(std::move(job));
    }
    ready_.notify_one();
    return result;
  }

private:
  struct Job {
    ObjectKey key;
    Bytes bytes;
    std::promise<Bytes> result;
  };
  void run(std::stop_token stop) {
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    while (!stop.stop_requested()) {
      std::optional<Job> job;
      {
        std::unique_lock lock(mutex_);
        if (!ready_.wait(lock, stop, [&] { return !jobs_.empty(); }))
          return;
        if (stop.stop_requested())
          return;
        job.emplace(std::move(jobs_.front()));
        jobs_.pop_front();
      }
      try {
        require(context != nullptr, "cannot allocate compression context");
        auto encoded = encoder_(context.get(), job->key, job->bytes);
        // Release raw payload before making its result visible to the writer.
        // The coordinator may immediately free that job's byte-budget credit.
        Bytes{}.swap(job->bytes);
        job->result.set_value(std::move(encoded));
      } catch (...) {
        job->result.set_exception(std::current_exception());
      }
    }
  }
  Encoder encoder_;
  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<Job> jobs_;
  // Last member: partial constructor failure joins workers before other members
  // die. Stop-aware waits allow their jthread destructors to wake and join.
  std::vector<std::jthread> workers_;
};
} // namespace syrnck
