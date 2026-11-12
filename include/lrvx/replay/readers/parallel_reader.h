/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/replay/readers/binary_log_reader.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace lrvx::replay
{

struct ParallelReaderConfig
{
  std::filesystem::path data_dir;
  uint32_t num_threads{0};
  size_t prefetch_segments{2};
  size_t event_buffer_size{10000};
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  std::set<uint32_t> symbols;
  bool verify_crc{true};
  bool sort_output{true};
};

struct ParallelReaderStats
{
  uint64_t segments_processed{0};
  uint64_t events_read{0};
  uint64_t trades_read{0};
  uint64_t book_updates_read{0};
  // Same definitions as the ReaderStats fields of the same names.
  uint64_t bytes_read{0};
  uint64_t crc_errors{0};
  uint64_t unknown_frames_skipped{0};

  int64_t start_time_ns{0};
  int64_t end_time_ns{0};

  double eventsPerSecond() const
  {
    if (end_time_ns <= start_time_ns)
    {
      return 0;
    }
    double elapsed_sec = static_cast<double>(end_time_ns - start_time_ns) / 1e9;
    return static_cast<double>(events_read) / elapsed_sec;
  }

  double throughputMBps() const
  {
    if (end_time_ns <= start_time_ns)
    {
      return 0;
    }
    double elapsed_sec = static_cast<double>(end_time_ns - start_time_ns) / 1e9;
    return static_cast<double>(bytes_read) / (1024.0 * 1024.0) / elapsed_sec;
  }
};

struct SegmentBuffer
{
  std::vector<ReplayEvent> events;
  SegmentInfo info;
  size_t current_index{0};
  // Event-data bytes read from the segment file (BinaryLogIterator::bytesConsumed).
  uint64_t bytes_read{0};
  // Frames the iterator skipped: payload CRC mismatch, unknown record type.
  uint64_t crc_errors{0};
  uint64_t unknown_frames_skipped{0};

  bool empty() const { return current_index >= events.size(); }
  const ReplayEvent& front() const { return events[current_index]; }
  void pop() { ++current_index; }
};

class ParallelReader
{
 public:
  explicit ParallelReader(ParallelReaderConfig config);
  ~ParallelReader();

  ParallelReader(const ParallelReader&) = delete;
  ParallelReader& operator=(const ParallelReader&) = delete;

  using EventCallback = std::function<bool(const ReplayEvent&)>;
  uint64_t forEach(EventCallback callback);

  using BatchCallback = std::function<bool(const std::vector<ReplayEvent>&)>;
  uint64_t forEachBatch(BatchCallback callback);

  template <typename Result>
  using SegmentProcessor = std::function<Result(const std::vector<ReplayEvent>&, const SegmentInfo&)>;

  template <typename Result>
  std::vector<Result> mapSegments(SegmentProcessor<Result> processor);

  ParallelReaderStats stats() const;
  const std::vector<SegmentInfo>& segments() const { return _segments; }
  uint32_t numThreads() const { return _num_threads; }

 private:
  void workerThread(size_t thread_id);
  std::unique_ptr<SegmentBuffer> readSegment(const SegmentInfo& segment);
  void mergeBuffers(std::vector<std::unique_ptr<SegmentBuffer>>& buffers, const EventCallback& callback);
  bool passesFilter(const ReplayEvent& event) const;
  // Adds one segment read to _stats: the segment, its bytes and skipped
  // frames, and the events it delivered.
  void addSegmentStats(const SegmentBuffer& buffer);

  ParallelReaderConfig _config;
  uint32_t _num_threads;

  std::vector<SegmentInfo> _segments;

  std::vector<std::thread> _workers;
  std::mutex _queue_mutex;
  std::condition_variable _queue_cv;
  std::queue<size_t> _work_queue;
  std::atomic<bool> _shutdown{false};

  std::mutex _results_mutex;
  std::condition_variable _results_cv;
  std::queue<std::unique_ptr<SegmentBuffer>> _results_queue;

  mutable ParallelReaderStats _stats;
};

template <typename Result>
std::vector<Result> ParallelReader::mapSegments(SegmentProcessor<Result> processor)
{
  std::vector<std::future<Result>> futures;
  futures.reserve(_segments.size());

  for (const auto& segment : _segments)
  {
    futures.push_back(std::async(
        std::launch::async,
        [this, &processor, &segment]()
        {
          auto buffer = readSegment(segment);
          if (buffer)
          {
            return processor(buffer->events, buffer->info);
          }
          return Result{};
        }));
  }

  std::vector<Result> results;
  results.reserve(futures.size());

  for (auto& future : futures)
  {
    results.push_back(future.get());
  }

  return results;
}

inline uint64_t parallelForEach(const std::filesystem::path& data_dir,
                                const ParallelReader::EventCallback& callback,
                                uint32_t num_threads = 0)
{
  ParallelReaderConfig config{.data_dir = data_dir, .num_threads = num_threads};
  ParallelReader reader(config);
  return reader.forEach(callback);
}

inline uint64_t parallelCount(const std::filesystem::path& data_dir, uint32_t num_threads = 0)
{
  std::atomic<uint64_t> count{0};
  parallelForEach(
      data_dir, [&count](const ReplayEvent&)
      {
        count.fetch_add(1, std::memory_order_relaxed);
        return true; },
      num_threads);
  return count.load();
}

}  // namespace lrvx::replay
