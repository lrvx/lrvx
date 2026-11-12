/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/readers/parallel_reader.h"

#include <algorithm>
#include <chrono>

namespace lrvx::replay
{

ParallelReader::ParallelReader(ParallelReaderConfig config) : _config(std::move(config))
{
  // Determine thread count
  _num_threads = _config.num_threads;
  if (_num_threads == 0)
  {
    _num_threads = std::thread::hardware_concurrency();
    if (_num_threads == 0)
    {
      _num_threads = 4;  // Fallback
    }
  }

  // Scan segments
  ReaderConfig reader_config{
      .data_dir = _config.data_dir,
      .from_ns = _config.from_ns,
      .to_ns = _config.to_ns,
      .symbols = _config.symbols,
      .verify_crc = _config.verify_crc,
  };

  // The order BinaryLogReader visits segments in: by proven first event where
  // the segment proves one, by file name otherwise. The header's
  // first_event_ns is arrival order on a compressed segment and proves
  // nothing without the Sorted flag.
  BinaryLogReader scanner(reader_config);
  _segments = scanner.segmentsInReadOrder();
}

ParallelReader::~ParallelReader()
{
  {
    // Store under the queue mutex: the workers' wait predicate reads _shutdown
    // with the lock held, so an unlocked store+notify can slip between the
    // predicate check and the sleep and the worker never wakes (join hangs).
    std::lock_guard lock(_queue_mutex);
    _shutdown.store(true);
  }
  _queue_cv.notify_all();

  for (auto& worker : _workers)
  {
    if (worker.joinable())
    {
      worker.join();
    }
  }
}

void ParallelReader::workerThread(size_t /*thread_id*/)
{
  while (true)
  {
    size_t segment_idx;

    {
      std::unique_lock lock(_queue_mutex);
      _queue_cv.wait(lock, [this]()
                     { return _shutdown.load() || !_work_queue.empty(); });

      if (_shutdown.load() && _work_queue.empty())
      {
        return;
      }

      segment_idx = _work_queue.front();
      _work_queue.pop();
    }

    // Read segment
    auto buffer = readSegment(_segments[segment_idx]);

    if (buffer)
    {
      std::lock_guard lock(_results_mutex);
      _results_queue.push(std::move(buffer));
      _results_cv.notify_one();
    }
  }
}

std::unique_ptr<SegmentBuffer> ParallelReader::readSegment(const SegmentInfo& segment)
{
  auto buffer = std::make_unique<SegmentBuffer>();
  buffer->info = segment;
  buffer->events.reserve(segment.event_count);

  BinaryLogIterator iter(segment.path);
  if (!iter.isValid())
  {
    return nullptr;
  }

  // Apply time range filter at seek level if possible
  if (_config.from_ns.has_value() && iter.hasIndex())
  {
    iter.seekToTimestamp(*_config.from_ns);
  }

  // Only a segment the writer marked Sorted is guaranteed monotonic, and only
  // then can the first event past the upper bound end the walk. On an
  // unsorted segment that shortcut dropped everything behind a single out-of
  // order frame, so this path disagreed with the single-threaded reader on
  // the same file.
  const bool ordered = iter.header().isSorted();

  ReplayEvent event;
  while (iter.next(event))
  {
    // Apply time range filter
    if (_config.from_ns.has_value() && event.timestamp_ns < *_config.from_ns)
    {
      continue;
    }
    if (_config.to_ns.has_value() && event.timestamp_ns > *_config.to_ns)
    {
      if (ordered)
      {
        break;
      }
      continue;
    }

    // Apply symbol filter
    if (!_config.symbols.empty())
    {
      uint32_t symbol_id = event.symbolId();
      if (_config.symbols.find(symbol_id) == _config.symbols.end())
      {
        continue;
      }
    }

    buffer->events.push_back(std::move(event));
  }

  buffer->bytes_read = iter.bytesConsumed();
  buffer->crc_errors = iter.crcErrors();
  buffer->unknown_frames_skipped = iter.unknownFramesSkipped();
  return buffer;
}

void ParallelReader::addSegmentStats(const SegmentBuffer& buffer)
{
  ++_stats.segments_processed;
  _stats.bytes_read += buffer.bytes_read;
  _stats.crc_errors += buffer.crc_errors;
  _stats.unknown_frames_skipped += buffer.unknown_frames_skipped;
  _stats.events_read += buffer.events.size();
  for (const auto& event : buffer.events)
  {
    event.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
  }
}

void ParallelReader::mergeBuffers(std::vector<std::unique_ptr<SegmentBuffer>>& buffers,
                                  const EventCallback& callback)
{
  // Min-heap on (timestamp, file-name order), the order BinaryLogReader
  // breaks ties in; each buffer is consumed front to back, which supplies the
  // last tie-break, position in the segment.
  auto cmp = [](const SegmentBuffer* a, const SegmentBuffer* b)
  {
    if (a->front().timestamp_ns != b->front().timestamp_ns)
    {
      return a->front().timestamp_ns > b->front().timestamp_ns;
    }
    return b->info.path < a->info.path;
  };

  std::priority_queue<SegmentBuffer*, std::vector<SegmentBuffer*>,
                      decltype(cmp)>
      heap(cmp);

  // Initialize heap with non-empty buffers
  for (auto& buffer : buffers)
  {
    if (buffer && !buffer->empty())
    {
      heap.push(buffer.get());
    }
  }

  // Merge
  while (!heap.empty())
  {
    SegmentBuffer* top = heap.top();
    heap.pop();

    if (!callback(top->front()))
    {
      return;  // Early exit requested
    }

    top->pop();

    if (!top->empty())
    {
      heap.push(top);
    }
  }
}

bool ParallelReader::passesFilter(const ReplayEvent& event) const
{
  if (_config.from_ns.has_value() && event.timestamp_ns < *_config.from_ns)
  {
    return false;
  }
  if (_config.to_ns.has_value() && event.timestamp_ns > *_config.to_ns)
  {
    return false;
  }
  if (!_config.symbols.empty())
  {
    uint32_t symbol_id =
        event.symbolId();
    if (_config.symbols.find(symbol_id) == _config.symbols.end())
    {
      return false;
    }
  }
  return true;
}

uint64_t ParallelReader::forEach(EventCallback callback)
{
  if (_segments.empty())
  {
    return 0;
  }

  _stats.start_time_ns = time_utils::nowNanos();

  // Small datasets and a single thread: BinaryLogReader::forEach, which
  // delivers Sorted segments in timestamp order across segments (overlapping
  // ones merged event by event) one segment at a time, and sorts each segment
  // without the Sorted flag in its file-name position. Concatenating segment
  // buffers instead left overlapping segments out of order.
  if (_segments.size() <= 2 || _num_threads == 1)
  {
    ReaderConfig reader_config{
        .data_dir = _config.data_dir,
        .from_ns = _config.from_ns,
        .to_ns = _config.to_ns,
        .symbols = _config.symbols,
        .verify_crc = _config.verify_crc,
    };
    BinaryLogReader reader(reader_config);
    uint64_t count = 0;
    reader.forEach([&count, &callback](const ReplayEvent& event)
                   {
                     if (!callback(event))
                     {
                       return false;
                     }
                     ++count;
                     return true; });
    const ReaderStats rs = reader.stats();
    _stats.segments_processed += rs.files_read;
    _stats.events_read += rs.events_read;
    _stats.trades_read += rs.trades_read;
    _stats.book_updates_read += rs.book_updates_read;
    _stats.bytes_read += rs.bytes_read;
    _stats.crc_errors += rs.crc_errors;
    _stats.unknown_frames_skipped += rs.unknown_frames_skipped;
    _stats.end_time_ns = time_utils::nowNanos();
    return count;
  }

  // Multi-threaded approach with merge sort
  if (_config.sort_output)
  {
    // Read all segments in parallel, then merge
    std::vector<std::future<std::unique_ptr<SegmentBuffer>>> futures;
    futures.reserve(_segments.size());

    for (const auto& segment : _segments)
    {
      futures.push_back(std::async(std::launch::async,
                                   [this, &segment]()
                                   { return readSegment(segment); }));
    }

    // Collect results
    std::vector<std::unique_ptr<SegmentBuffer>> buffers;
    buffers.reserve(futures.size());

    for (auto& future : futures)
    {
      auto buffer = future.get();
      if (buffer && !buffer->info.sorted)
      {
        // The merge needs every buffer in timestamp order, and only a Sorted
        // segment arrives that way.
        std::stable_sort(buffer->events.begin(), buffer->events.end(),
                         [](const ReplayEvent& a, const ReplayEvent& b)
                         { return a.timestamp_ns < b.timestamp_ns; });
      }
      if (buffer)
      {
        addSegmentStats(*buffer);
      }
      buffers.push_back(std::move(buffer));
    }

    // Merge and deliver events
    uint64_t count = 0;
    EventCallback counting_callback = [&count, &callback](const ReplayEvent& event)
    {
      ++count;
      return callback(event);
    };

    mergeBuffers(buffers, counting_callback);
    _stats.end_time_ns = time_utils::nowNanos();
    return count;
  }
  else
  {
    // Deliver events as they become available (no sorting)
    std::atomic<uint64_t> count{0};
    std::atomic<bool> stop_requested{false};

    std::vector<std::future<void>> futures;
    futures.reserve(_segments.size());

    // Held while delivering a segment's events and while adding its stats, so
    // the callback is never entered concurrently and _stats is written by one
    // thread at a time.
    std::mutex callback_mutex;

    for (const auto& segment : _segments)
    {
      futures.push_back(std::async(std::launch::async,
                                   [this, &segment, &callback, &count, &stop_requested,
                                    &callback_mutex]()
                                   {
                                     if (stop_requested.load())
                                     {
                                       return;
                                     }

                                     auto buffer = readSegment(segment);
                                     if (!buffer)
                                     {
                                       return;
                                     }
                                     {
                                       std::lock_guard lock(callback_mutex);
                                       addSegmentStats(*buffer);
                                     }

                                     for (const auto& event : buffer->events)
                                     {
                                       if (stop_requested.load())
                                       {
                                         return;
                                       }

                                       std::lock_guard lock(callback_mutex);
                                       if (!callback(event))
                                       {
                                         stop_requested.store(true);
                                         return;
                                       }
                                       count.fetch_add(1, std::memory_order_relaxed);
                                     }
                                   }));
    }

    // Wait for all
    for (auto& future : futures)
    {
      future.wait();
    }

    _stats.end_time_ns = time_utils::nowNanos();
    return count.load();
  }
}

uint64_t ParallelReader::forEachBatch(BatchCallback callback)
{
  if (_segments.empty())
  {
    return 0;
  }

  _stats.start_time_ns = time_utils::nowNanos();

  std::atomic<uint64_t> total_events{0};
  std::atomic<bool> stop_requested{false};
  std::mutex stats_mutex;

  std::vector<std::future<void>> futures;
  futures.reserve(_segments.size());

  for (const auto& segment : _segments)
  {
    futures.push_back(std::async(std::launch::async,
                                 [this, &segment, &callback, &total_events, &stop_requested,
                                  &stats_mutex]()
                                 {
                                   if (stop_requested.load())
                                   {
                                     return;
                                   }

                                   auto buffer = readSegment(segment);
                                   if (!buffer)
                                   {
                                     return;
                                   }
                                   {
                                     std::lock_guard lock(stats_mutex);
                                     addSegmentStats(*buffer);
                                   }
                                   if (buffer->events.empty())
                                   {
                                     return;
                                   }

                                   total_events.fetch_add(buffer->events.size(),
                                                          std::memory_order_relaxed);

                                   if (!callback(buffer->events))
                                   {
                                     stop_requested.store(true);
                                   }
                                 }));
  }

  // Wait for all
  for (auto& future : futures)
  {
    future.wait();
  }

  _stats.end_time_ns = time_utils::nowNanos();
  return total_events.load();
}

ParallelReaderStats ParallelReader::stats() const
{
  return _stats;
}

}  // namespace lrvx::replay
