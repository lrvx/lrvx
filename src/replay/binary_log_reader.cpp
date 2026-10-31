/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/error/lrvx_error.h"
#include "lrvx/log/log.h"
#include "lrvx/replay/aggregator.h"
#include "lrvx/replay/ops/compression.h"
#include "lrvx/util/concurrency/thread_body.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lrvx::replay
{

namespace
{

// Extract the first non-zero timestamp from a decompressed block buffer.
int64_t firstTimestampInBlock(const std::vector<std::byte>& data)
{
  size_t offset = 0;
  while (offset + sizeof(FrameHeader) <= data.size())
  {
    FrameHeader frame;
    std::memcpy(&frame, data.data() + offset, sizeof(frame));
    offset += sizeof(FrameHeader);
    if (offset + frame.size > data.size())
    {
      break;
    }

    int64_t ts = 0;
    auto type = static_cast<EventType>(frame.type);
    if (type == EventType::Trade && frame.size >= sizeof(TradeRecord))
    {
      TradeRecord tr;
      std::memcpy(&tr, data.data() + offset, sizeof(TradeRecord));
      ts = tr.exchange_ts_ns;
    }
    else if ((type == EventType::BookSnapshot || type == EventType::BookDelta ||
              type == EventType::BookTop) &&
             frame.size >= sizeof(BookRecordHeader))
    {
      BookRecordHeader bh;
      std::memcpy(&bh, data.data() + offset, sizeof(BookRecordHeader));
      ts = bh.exchange_ts_ns;
    }
    else if (type == EventType::OptionQuote && frame.size >= sizeof(OptionQuoteRecord))
    {
      OptionQuoteRecord oq;
      std::memcpy(&oq, data.data() + offset, sizeof(OptionQuoteRecord));
      ts = oq.exchange_ts_ns;
    }
    else if (type == EventType::PoolState && frame.size >= sizeof(PoolStateRecordHeader))
    {
      PoolStateRecordHeader ph;
      std::memcpy(&ph, data.data() + offset, sizeof(PoolStateRecordHeader));
      ts = ph.exchange_ts_ns;
    }
    if (ts > 0)
    {
      return ts;
    }

    offset += frame.size;
  }
  return 0;
}

// Extract the last non-zero timestamp from a decompressed block buffer.
int64_t lastTimestampInBlock(const std::vector<std::byte>& data)
{
  int64_t last = 0;
  size_t offset = 0;
  while (offset + sizeof(FrameHeader) <= data.size())
  {
    FrameHeader frame;
    std::memcpy(&frame, data.data() + offset, sizeof(frame));
    offset += sizeof(FrameHeader);
    if (offset + frame.size > data.size())
    {
      break;
    }

    int64_t ts = 0;
    auto type = static_cast<EventType>(frame.type);
    if (type == EventType::Trade && frame.size >= sizeof(TradeRecord))
    {
      TradeRecord tr;
      std::memcpy(&tr, data.data() + offset, sizeof(TradeRecord));
      ts = tr.exchange_ts_ns;
    }
    else if ((type == EventType::BookSnapshot || type == EventType::BookDelta ||
              type == EventType::BookTop) &&
             frame.size >= sizeof(BookRecordHeader))
    {
      BookRecordHeader bh;
      std::memcpy(&bh, data.data() + offset, sizeof(BookRecordHeader));
      ts = bh.exchange_ts_ns;
    }
    else if (type == EventType::OptionQuote && frame.size >= sizeof(OptionQuoteRecord))
    {
      OptionQuoteRecord oq;
      std::memcpy(&oq, data.data() + offset, sizeof(OptionQuoteRecord));
      ts = oq.exchange_ts_ns;
    }
    else if (type == EventType::PoolState && frame.size >= sizeof(PoolStateRecordHeader))
    {
      PoolStateRecordHeader ph;
      std::memcpy(&ph, data.data() + offset, sizeof(PoolStateRecordHeader));
      ts = ph.exchange_ts_ns;
    }
    if (ts > 0)
    {
      last = ts;
    }

    offset += frame.size;
  }
  return last;
}

// Metadata for one compressed block — used by intra-segment parallel
// workers to know where each block lives and how big it is. Filled
// by `scanBlockOffsetsForSegment` via a fast header-only walk (no
// decompression).
struct BlockMeta
{
  uint64_t file_offset;  // position of the CompressedBlockHeader in the file
  uint64_t data_offset;  // position of the compressed payload (header + 16)
  uint32_t comp_size;    // compressed payload length
  uint32_t orig_size;    // decompressed payload length
  uint32_t event_count;  // events in this block (from the header)
};

// Walks a compressed segment reading only CompressedBlockHeaders and
// skipping past each block's payload by its `compressed_size`. No
// decompression. Stops at index trailer if present, otherwise at the
// first invalid header or EOF.
//
// Returns true if at least one valid block was discovered (false
// only on outright IO failure or invalid segment header). Callers
// can treat an empty out_blocks as "process this segment single-
// threaded".
inline bool scanBlockOffsetsForSegment(const std::filesystem::path& path,
                                       SegmentHeader& out_hdr,
                                       std::vector<BlockMeta>& out_blocks)
{
  out_blocks.clear();
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f)
  {
    return false;
  }

  if (std::fread(&out_hdr, sizeof(out_hdr), 1, f) != 1 || !out_hdr.isValid())
  {
    std::fclose(f);
    return false;
  }
  if (!out_hdr.isCompressed())
  {
    // Uncompressed segments are walked directly; intra-segment
    // parallelism for them isn't wired up here.
    std::fclose(f);
    return true;
  }

  if (std::fseek(f, 0, SEEK_END) != 0)
  {
    std::fclose(f);
    return false;
  }
  const long file_end = std::ftell(f);
  if (file_end < 0)
  {
    std::fclose(f);
    return false;
  }
  const long stop_at =
      (out_hdr.hasIndex() && out_hdr.index_offset > 0)
          ? static_cast<long>(out_hdr.index_offset)
          : file_end;

  if (std::fseek(f, sizeof(SegmentHeader), SEEK_SET) != 0)
  {
    std::fclose(f);
    return false;
  }

  for (;;)
  {
    const long pos = std::ftell(f);
    if (pos < 0 || pos >= stop_at)
    {
      break;
    }
    CompressedBlockHeader bh;
    if (std::fread(&bh, sizeof(bh), 1, f) != 1 || !bh.isValid())
    {
      break;
    }
    const long data_pos = std::ftell(f);
    if (data_pos < 0 ||
        static_cast<long>(bh.compressed_size) > stop_at - data_pos)
    {
      // Truncated tail (live-tape write in progress) — stop here.
      break;
    }
    out_blocks.push_back(BlockMeta{static_cast<uint64_t>(pos),
                                   static_cast<uint64_t>(data_pos),
                                   bh.compressed_size, bh.original_size,
                                   bh.event_count});
    if (std::fseek(f, data_pos + static_cast<long>(bh.compressed_size),
                   SEEK_SET) != 0)
    {
      break;
    }
  }

  std::fclose(f);
  return true;
}

// In-place sort of frames inside a decompressed block by exchange_ts_ns.
// Mirrors `BinaryLogWriter::sortBlockBuffer`: walks frames, collects
// (offset, total_size, timestamp) refs, fast-paths monotonic blocks
// (no-op), and stable-sorts on the refs writing into a scratch buffer
// that's then swapped with the input. Memory: O(events_in_block × 24
// bytes for refs) + O(block_size) for the scratch buffer.
//
// For Sorted=true segments this fast-paths because the writer already
// did the same sort on the producing side; cost is one linear scan to
// confirm monotonicity. For Sorted=false segments (md_collector and
// other external writers) this lifts intra-block jitter (typically
// 1-2ms exchange-WS jitter, observed 95th-percentile of OOO deltas
// in production traces) without any caller involvement.
inline void sortBlockFramesInPlace(std::vector<std::byte>& block_data,
                                   uint32_t event_count,
                                   std::vector<std::byte>& scratch)
{
  struct FrameRef
  {
    size_t offset;
    size_t total_size;
    int64_t timestamp;
  };

  std::vector<FrameRef> refs;
  refs.reserve(event_count);

  size_t pos = 0;
  while (pos < block_data.size())
  {
    if (pos + sizeof(FrameHeader) > block_data.size())
    {
      break;
    }
    FrameHeader hdr;
    std::memcpy(&hdr, block_data.data() + pos, sizeof(FrameHeader));
    const size_t total = sizeof(FrameHeader) + hdr.size;
    if (pos + total > block_data.size())
    {
      break;
    }
    int64_t ts = 0;
    if (hdr.size >= sizeof(int64_t))
    {
      // exchange_ts_ns is the first field of both TradeRecord and
      // BookRecordHeader.
      std::memcpy(&ts, block_data.data() + pos + sizeof(FrameHeader),
                  sizeof(int64_t));
    }
    refs.push_back({pos, total, ts});
    pos += total;
  }

  // Fast path: already monotonic. No buffer copy.
  bool sorted = true;
  for (size_t i = 1; i < refs.size(); ++i)
  {
    if (refs[i].timestamp < refs[i - 1].timestamp)
    {
      sorted = false;
      break;
    }
  }
  if (sorted)
  {
    return;
  }

  std::stable_sort(refs.begin(), refs.end(),
                   [](const FrameRef& a, const FrameRef& b)
                   { return a.timestamp < b.timestamp; });

  scratch.resize(block_data.size());
  size_t out_pos = 0;
  for (const auto& ref : refs)
  {
    std::memcpy(scratch.data() + out_pos, block_data.data() + ref.offset,
                ref.total_size);
    out_pos += ref.total_size;
  }
  std::swap(block_data, scratch);
}

// Decompress a single block at a known file position.
//
// Returns {} on any inconsistency (truncated block, implausible header sizes,
// failed decompression). Important for live tapes: the actively-written tail
// block can have a header on disk before its payload is finished, leaving
// `compressed_size` pointing past EOF or — worse, if the header bytes weren't
// yet written and contain prior garbage — to a value in the gigabytes that
// would OOM the recovery path.
std::vector<std::byte> decompressBlock(std::FILE* f, long data_pos,
                                       const CompressedBlockHeader& block,
                                       CompressionType comp_type)
{
  if (std::fseek(f, 0, SEEK_END) != 0)
  {
    return {};
  }
  long file_end = std::ftell(f);
  if (file_end < 0 || data_pos < 0 || data_pos > file_end)
  {
    return {};
  }
  if (block.compressed_size == 0 ||
      block.compressed_size > static_cast<uint32_t>(file_end - data_pos))
  {
    return {};
  }

  std::vector<std::byte> compressed(block.compressed_size);
  if (std::fseek(f, data_pos, SEEK_SET) != 0)
  {
    return {};
  }
  if (std::fread(compressed.data(), 1, block.compressed_size, f) != block.compressed_size)
  {
    return {};
  }
  return Compressor::decompress(comp_type, std::span<const std::byte>(compressed),
                                block.original_size);
}

// For compressed segments where the writer did not finalize the segment header
// (event_count/first_event_ns/last_event_ns are all 0), recover these fields by:
//   - scanning all CompressedBlockHeaders to sum event_count (no decompression)
//   - decompressing only the necessary blocks for timestamps
//
// The last block of an active segment is often truncated (the writer flushes the
// block header before completing the compressed data write). We therefore iterate
// backwards from the end to find the last block that decompresses successfully.
// Record types this build can parse. A frame carrying anything else is a
// record from a newer writer: the format is additive, so it is skipped by
// FrameHeader.size rather than ending the read.
inline bool isKnownEventType(uint8_t type) noexcept
{
  switch (static_cast<EventType>(type))
  {
    case EventType::Trade:
    case EventType::BookSnapshot:
    case EventType::BookDelta:
    case EventType::BookTop:
    case EventType::OptionQuote:
    case EventType::PoolState:
      return true;
  }
  return false;
}

// Largest frame payload the readers accept. Anything larger is a corrupt
// length field, not a record.
inline constexpr uint32_t kMaxFrameBytes = 10u * 1024u * 1024u;

// Recovers the counters of an uncompressed segment whose header still reads
// zero -- the state a collector leaves behind when it is killed before close.
// The frames themselves are intact and self-describing, so a straight walk of
// the frame chain gives back the count and the time span. Without this the
// segment is invisible to every range read: the header says it ends at 0, so
// the binary search over segments skips it and the caller gets a silent
// "ok, no events".
void recoverUncompressedSegmentMeta(std::FILE* f, const SegmentHeader& hdr,
                                    uint32_t& out_event_count,
                                    int64_t& out_first_ns, int64_t& out_last_ns)
{
  out_event_count = 0;
  out_first_ns = 0;
  out_last_ns = 0;

  if (std::fseek(f, 0, SEEK_END) != 0)
  {
    return;
  }
  const long file_end = std::ftell(f);
  if (file_end < 0)
  {
    return;
  }

  const long stop_at = (hdr.hasIndex() && hdr.index_offset > 0)
                           ? std::min<long>(file_end, static_cast<long>(hdr.index_offset))
                           : file_end;

  if (std::fseek(f, sizeof(SegmentHeader), SEEK_SET) != 0)
  {
    return;
  }

  std::vector<std::byte> payload;
  for (;;)
  {
    const long pos = std::ftell(f);
    if (pos < 0 || pos + static_cast<long>(sizeof(FrameHeader)) > stop_at)
    {
      break;
    }

    FrameHeader frame{};
    if (std::fread(&frame, sizeof(frame), 1, f) != 1)
    {
      break;
    }
    if (frame.size > kMaxFrameBytes)
    {
      break;
    }
    const long data_pos = std::ftell(f);
    if (data_pos < 0 || static_cast<long>(frame.size) > stop_at - data_pos)
    {
      // Half-written frame at the tail of a live segment.
      break;
    }

    // Every record type starts with exchange_ts_ns, so the first int64 of the
    // payload is the timestamp whatever the type is.
    int64_t ts = 0;
    if (frame.size >= sizeof(int64_t))
    {
      if (std::fread(&ts, sizeof(ts), 1, f) != 1)
      {
        break;
      }
    }

    if (out_event_count == 0)
    {
      out_first_ns = ts;
    }
    out_last_ns = ts;
    ++out_event_count;

    if (std::fseek(f, data_pos + static_cast<long>(frame.size), SEEK_SET) != 0)
    {
      break;
    }
  }
}

void recoverCompressedSegmentMeta(std::FILE* f, const SegmentHeader& hdr,
                                  uint32_t& out_event_count,
                                  int64_t& out_first_ns, int64_t& out_last_ns)
{
  out_event_count = 0;
  out_first_ns = 0;
  out_last_ns = 0;

  if (std::fseek(f, 0, SEEK_END) != 0)
  {
    return;
  }
  const long file_end = std::ftell(f);
  if (file_end < 0)
  {
    return;
  }

  const long stop_at =
      std::min<long>(file_end, (hdr.hasIndex() && hdr.index_offset > 0)
                                   ? static_cast<long>(hdr.index_offset)
                                   : std::numeric_limits<long>::max());

  if (std::fseek(f, sizeof(SegmentHeader), SEEK_SET) != 0)
  {
    return;
  }

  // Scan all block headers: count events, collect positions of non-empty blocks.
  // Stops cleanly on the live-tape tail: a partially-written block header is
  // either invalid (caught by `block.isValid()`) or claims a `compressed_size`
  // past EOF (caught below). Without the EOF check, a stale `compressed_size`
  // in unflushed header bytes can lead recoverCompressedSegmentMeta to allocate
  // gigabytes when the recovery path later tries to read the block.
  struct BlockLoc
  {
    long data_pos;
    CompressedBlockHeader hdr;
  };
  std::vector<BlockLoc> blocks;

  for (;;)
  {
    long pos = std::ftell(f);
    if (pos < 0 || pos >= stop_at)
    {
      break;
    }

    CompressedBlockHeader block;
    if (std::fread(&block, sizeof(block), 1, f) != 1 || !block.isValid())
    {
      break;
    }

    long data_pos = std::ftell(f);
    if (data_pos < 0 ||
        static_cast<long>(block.compressed_size) > stop_at - data_pos)
    {
      // Block claims to extend past the file (or past the index trailer) —
      // tail of a live-writing segment. Don't include it in the event count
      // or the decompression list.
      break;
    }
    out_event_count += block.event_count;

    if (block.event_count > 0)
    {
      blocks.push_back({data_pos, block});
    }

    if (std::fseek(f, data_pos + static_cast<long>(block.compressed_size), SEEK_SET) != 0)
    {
      break;
    }
  }

  if (blocks.empty())
  {
    return;
  }

  auto comp_type = hdr.compressionType();

  // first_ns: try blocks forward until one decompresses.
  for (const auto& bl : blocks)
  {
    auto data = decompressBlock(f, bl.data_pos, bl.hdr, comp_type);
    if (!data.empty())
    {
      int64_t ts = firstTimestampInBlock(data);
      if (ts > 0)
      {
        out_first_ns = ts;
        break;
      }
    }
  }

  // last_ns: try blocks backward — the last block is often truncated.
  for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; --i)
  {
    auto data = decompressBlock(f, blocks[i].data_pos, blocks[i].hdr, comp_type);
    if (!data.empty())
    {
      int64_t ts = lastTimestampInBlock(data);
      if (ts > 0)
      {
        out_last_ns = ts;
        break;
      }
    }
  }
}

// Min-heap comparator for ReplayEvent. Greater-than yields a min-heap
// when paired with std::push_heap / std::pop_heap. Defined in this
// anon namespace so both `streamSegmentWithStats` (single-thread per-
// segment loop) and the worker-mode `streamBlockRangeImpl` template
// below can use it.
struct ReplayEventLater
{
  bool operator()(const ReplayEvent& a, const ReplayEvent& b) const noexcept
  {
    return a.timestamp_ns > b.timestamp_ns;
  }
};

// Bounded reorder buffer shared by every streaming path.
//
// Three things it fixes over the loop it replaces. The filter runs before the
// window check, so an event belonging to a symbol nobody asked to replay can
// no longer move the watermark and kill the run. A late event is dropped and
// counted instead of thrown: a run that discards fifteen frames out of twelve
// million and says so is worth more than a run that dies halfway with
// half-fed aggregators and nothing to roll back to. And the watermark can be
// owned by the caller, so an inversion that straddles a segment boundary is
// visible instead of silently resetting.
//
// ReaderConfig::strict_ordering brings the throw back for reproducibility
// work, now with the symbol, event type, file and offset in the message.
template <typename Filter>
class ReorderBuffer
{
 public:
  ReorderBuffer(BinaryLogReader::EventCallback& callback, ReaderStats& stats,
                int64_t window_ns, bool strict, Filter passes_filter,
                int64_t& watermark, const std::filesystem::path& source)
      : _callback(callback),
        _stats(stats),
        _window_ns(window_ns),
        _strict(strict),
        _passes(std::move(passes_filter)),
        _watermark(watermark),
        _source(source)
  {
  }

  // Returns false when the callback asked to stop.
  bool offer(ReplayEvent&& event, uint64_t file_offset)
  {
    if (!_passes(event))
    {
      return true;
    }

    if (_watermark != std::numeric_limits<int64_t>::min() &&
        event.timestamp_ns < _watermark - _window_ns)
    {
      const int64_t delta = _watermark - event.timestamp_ns;
      if (_strict)
      {
        throw lrvx::LrvxError("E_DATA_002", lateMessage(event, delta, file_offset));
      }
      ++_stats.late_dropped;
      if (!_warned)
      {
        _warned = true;
        LRVX_LOG_WARN(lateMessage(event, delta, file_offset)
                      << " Dropping it and any further late events in this segment; "
                         "see ReaderStats::late_dropped for the total.");
      }
      return true;
    }

    if (event.timestamp_ns > _watermark)
    {
      _watermark = event.timestamp_ns;
    }
    _heap.push_back(std::move(event));
    std::push_heap(_heap.begin(), _heap.end(), _cmp);

    return drainBelow(_watermark - _window_ns);
  }

  bool drainAll() { return drainBelow(std::numeric_limits<int64_t>::max()); }

 private:
  bool drainBelow(int64_t threshold_ts_ns)
  {
    while (!_heap.empty() && _heap.front().timestamp_ns < threshold_ts_ns)
    {
      std::pop_heap(_heap.begin(), _heap.end(), _cmp);
      ReplayEvent ev = std::move(_heap.back());
      _heap.pop_back();
      ++_stats.events_read;
      ev.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
      if (!_callback(ev))
      {
        return false;
      }
    }
    return true;
  }

  std::string lateMessage(const ReplayEvent& event, int64_t delta, uint64_t file_offset) const
  {
    return "Event arrived past reorder window: delta=" + std::to_string(delta) +
           "ns exceeds reorder_window_ns=" + std::to_string(_window_ns) +
           " (symbol_id=" + std::to_string(event.symbolId()) +
           ", event_type=" + std::to_string(static_cast<int>(event.type)) +
           ", file=" + _source.string() + ", offset=" + std::to_string(file_offset) + ").";
  }

  BinaryLogReader::EventCallback& _callback;
  ReaderStats& _stats;
  int64_t _window_ns;
  bool _strict;
  Filter _passes;
  int64_t& _watermark;
  std::filesystem::path _source;

  std::vector<ReplayEvent> _heap;
  ReplayEventLater _cmp;
  bool _warned{false};
};

// Worker-mode segment processor for intra-segment parallel run().
// Opens a private BinaryLogIterator at `path`, switches it to
// stop-at-block-end mode, and walks the assigned `blocks` range:
// seekToBlockOffset → drain events → seek to next block. Events flow
// through the same bounded reorder buffer logic as
// streamSegmentWithStats so cross-block jitter within the worker's
// range is correctly merged. Boundary effects at the partition seam
// between workers are documented at IAggregator::merge (sliding-
// window aggregators may underreport on that seam).
template <typename Filter>
bool streamBlockRangeImpl(const std::filesystem::path& path,
                          std::span<const BlockMeta> blocks,
                          BinaryLogReader::EventCallback& callback,
                          ReaderStats& stats, int64_t reorder_window_ns,
                          bool strict_ordering, Filter passes_filter)
{
  if (blocks.empty())
  {
    return true;
  }
  BinaryLogIterator iter(path);
  if (!iter.isValid())
  {
    return false;
  }
  iter.setStopAtBlockEnd(true);

  ++stats.files_read;

  const bool is_sorted = iter.header().isSorted();

  if (is_sorted)
  {
    for (const auto& bm : blocks)
    {
      if (!iter.seekToBlockOffset(bm.file_offset))
      {
        return false;
      }
      ReplayEvent event;
      while (iter.next(event))
      {
        if (!passes_filter(event))
        {
          continue;
        }
        ++stats.events_read;
        event.type == EventType::Trade ? ++stats.trades_read : ++stats.book_updates_read;
        if (!callback(event))
        {
          return false;
        }
      }
    }
    return true;
  }

  // Unsorted: cross-block bounded reorder buffer over the worker's
  // assigned block range.
  int64_t watermark = std::numeric_limits<int64_t>::min();
  ReorderBuffer<Filter> reorder(callback, stats, reorder_window_ns, strict_ordering,
                                passes_filter, watermark, path);

  for (const auto& bm : blocks)
  {
    if (!iter.seekToBlockOffset(bm.file_offset))
    {
      return false;
    }
    ReplayEvent event;
    while (iter.next(event))
    {
      if (!reorder.offer(std::move(event), iter.lastEventOffset()))
      {
        return false;
      }
    }
  }

  return reorder.drainAll();
}

}  // namespace

BinaryLogReader::BinaryLogReader(ReaderConfig config) : _config(std::move(config)) {}

BinaryLogReader::~BinaryLogReader() = default;

BinaryLogReader::BinaryLogReader(BinaryLogReader&& other) noexcept
    : _config(std::move(other._config)),
      _stats(other._stats),
      _segments(std::move(other._segments)),
      _scanned(other._scanned),
      _progress_cb(std::move(other._progress_cb)),
      _progress_interval(other._progress_interval)
{
}

BinaryLogReader& BinaryLogReader::operator=(BinaryLogReader&& other) noexcept
{
  if (this != &other)
  {
    _config = std::move(other._config);
    _stats = other._stats;
    _segments = std::move(other._segments);
    _scanned = other._scanned;
    _progress_cb = std::move(other._progress_cb);
    _progress_interval = other._progress_interval;
  }
  return *this;
}

void BinaryLogReader::setProgressCallback(
    ProgressCallback cb, std::chrono::milliseconds interval)
{
  _progress_cb = std::move(cb);
  _progress_interval = (interval.count() > 0) ? interval : std::chrono::milliseconds(1000);
}

void BinaryLogReader::clearProgressCallback()
{
  _progress_cb = nullptr;
}

bool BinaryLogReader::ensureScanned() const
{
  std::call_once(_scan_once,
                 [this]
                 { const_cast<BinaryLogReader*>(this)->scanSegments(); });
  return _scanned;
}

bool BinaryLogReader::scanSegments()
{
  if (_scanned)
  {
    return true;
  }

  namespace fs = std::filesystem;

  if (!fs::exists(_config.data_dir))
  {
    return false;
  }

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : fs::directory_iterator(_config.data_dir))
  {
    if (entry.is_regular_file() && isSegmentExtension(entry.path().extension().string()))
    {
      paths.push_back(entry.path());
    }
  }

  // Sort by filename (which is timestamp-based)
  std::sort(paths.begin(), paths.end());

  // Read segment headers to build metadata
  for (const auto& path : paths)
  {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
    {
      continue;
    }

    SegmentHeader header;
    if (std::fread(&header, sizeof(header), 1, f) == 1 && header.isValid())
    {
      SegmentInfo info;
      info.path = path;
      info.first_event_ns = header.first_event_ns;
      info.last_event_ns = header.last_event_ns;
      info.event_count = header.event_count;
      info.has_index = header.hasIndex();
      info.index_offset = header.index_offset;

      if (header.event_count == 0)
      {
        if (header.isCompressed())
        {
          recoverCompressedSegmentMeta(f, header,
                                       info.event_count,
                                       info.first_event_ns,
                                       info.last_event_ns);
        }
        else
        {
          recoverUncompressedSegmentMeta(f, header,
                                         info.event_count,
                                         info.first_event_ns,
                                         info.last_event_ns);
        }
      }

      _segments.push_back(std::move(info));
    }
    std::fclose(f);
  }

  _scanned = true;
  return true;
}

bool BinaryLogReader::passesFilter(const ReplayEvent& event) const
{
  // Time filter
  if (_config.from_ns && event.timestamp_ns < *_config.from_ns)
  {
    return false;
  }
  if (_config.to_ns && event.timestamp_ns > *_config.to_ns)
  {
    return false;
  }

  // Symbol filter
  if (!_config.symbols.empty())
  {
    uint32_t symbol_id = event.symbolId();
    if (_config.symbols.find(symbol_id) == _config.symbols.end())
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::readSegment(const std::filesystem::path& path, EventCallback& callback)
{
  BinaryLogIterator iter(path);
  if (!iter.isValid())
  {
    return false;
  }

  ++_stats.files_read;

  if (iter.header().isSorted())
  {
    // Segment is guaranteed monotonic — stream directly without buffering.
    ReplayEvent event;
    while (iter.next(event))
    {
      if (!passesFilter(event))
      {
        continue;
      }
      ++_stats.events_read;
      if (event.type == EventType::Trade)
      {
        ++_stats.trades_read;
      }
      else
      {
        ++_stats.book_updates_read;
      }
      if (!callback(event))
      {
        return false;
      }
    }
    return true;
  }

  // Legacy path: segment has no ordering guarantee — buffer, sort, deliver.
  std::vector<ReplayEvent> events;
  ReplayEvent event;
  while (iter.next(event))
  {
    if (passesFilter(event))
    {
      events.push_back(std::move(event));
    }
  }

  std::stable_sort(events.begin(), events.end(),
                   [](const ReplayEvent& a, const ReplayEvent& b)
                   { return a.timestamp_ns < b.timestamp_ns; });

  for (const auto& e : events)
  {
    ++_stats.events_read;
    if (e.type == EventType::Trade)
    {
      ++_stats.trades_read;
    }
    else
    {
      ++_stats.book_updates_read;
    }
    if (!callback(e))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::readSegmentFrom(const SegmentInfo& segment, int64_t start_ts_ns,
                                      EventCallback& callback)
{
  BinaryLogIterator iter(segment.path);
  if (!iter.isValid())
  {
    return false;
  }

  ++_stats.files_read;

  // Use index if available to seek to starting position
  if (segment.has_index)
  {
    iter.loadIndex();
    if (iter.hasIndex())
    {
      iter.seekToTimestamp(start_ts_ns);
    }
  }

  ReplayEvent event;

  if (iter.header().isSorted())
  {
    while (iter.next(event))
    {
      if (event.timestamp_ns < start_ts_ns)
      {
        continue;
      }
      if (!passesFilter(event))
      {
        continue;
      }
      ++_stats.events_read;
      event.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
      if (!callback(event))
      {
        return false;
      }
    }
    return true;
  }

  // Legacy path: buffer, sort, deliver.
  std::vector<ReplayEvent> events;
  while (iter.next(event))
  {
    if (event.timestamp_ns < start_ts_ns)
    {
      continue;
    }
    if (passesFilter(event))
    {
      events.push_back(std::move(event));
    }
  }

  std::stable_sort(events.begin(), events.end(),
                   [](const ReplayEvent& a, const ReplayEvent& b)
                   { return a.timestamp_ns < b.timestamp_ns; });

  for (const auto& e : events)
  {
    ++_stats.events_read;
    e.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
    if (!callback(e))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::forEach(EventCallback callback)
{
  if (!ensureScanned())
  {
    return false;
  }

  for (const auto& segment : _segments)
  {
    if (!readSegment(segment.path, callback))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::run(std::span<IAggregator* const> aggregators,
                          std::size_t n_threads)
{
  // Empty span: do not even scan / decompress. This is the contract
  // that lets a `run([])` call cost effectively zero in the framework
  // tests that exercise the no-op path.
  if (aggregators.empty())
  {
    return true;
  }

  // Explicit single-thread path. Routes through streamForEach so
  // segments without the Sorted flag stay O(1) memory thanks to the
  // bounded reorder buffer.
  if (n_threads == 1)
  {
    // Progress accounting: when the callback is set, we approximate
    // pct by event count rather than bytes — we already know the
    // total event count from the manifest (or from a scan), but
    // pulling segment file sizes is cheap, so use bytes-processed as
    // the more meaningful pct. Without a scan we still fall back to
    // event counts.
    const auto* cb_ptr = _progress_cb ? &_progress_cb : nullptr;
    uint64_t total_events = 0;
    if (cb_ptr != nullptr)
    {
      // Best-effort cheap scan; if scanSegments fails (corrupt /
      // missing directory) we leave total_events = 0 and report
      // pct=0.0 to the callback. The walk itself will then fail and
      // surface the underlying error to the caller.
      if (ensureScanned())
      {
        for (const auto& seg : _segments)
        {
          total_events += seg.event_count;
        }
      }
    }

    uint64_t seen_events = 0;
    auto last_progress = std::chrono::steady_clock::now();
    bool cancelled = false;
    const bool walked = streamForEach(
        [&](const ReplayEvent& ev) -> bool
        {
          for (auto* agg : aggregators)
          {
            if (agg != nullptr)
            {
              agg->onEvent(ev);
            }
          }
          if (cb_ptr != nullptr)
          {
            ++seen_events;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= _progress_interval)
            {
              last_progress = now;
              const double pct =
                  total_events > 0
                      ? std::min(1.0, static_cast<double>(seen_events) /
                                          static_cast<double>(total_events))
                      : 0.0;
              if (!(*cb_ptr)(pct, ev.timestamp_ns))
              {
                cancelled = true;
                return false;  // stops streamForEach
              }
            }
          }
          return true;
        });

    for (auto* agg : aggregators)
    {
      if (agg != nullptr)
      {
        agg->finalize();
      }
    }

    if (cb_ptr != nullptr && walked && !cancelled)
    {
      // One final 100% tick so progress UIs reach the end frame. Use
      // the last segment's last_event_ns (we already scanned for the
      // total event count) so the timestamp is exact, not just the
      // most recent intra-walk dispatch.
      const int64_t end_ts =
          _segments.empty() ? int64_t{0} : _segments.back().last_event_ns;
      (*cb_ptr)(1.0, end_ts);
    }

    return walked && !cancelled;
  }

  // Auto / multi-thread path. Resolve n_threads after scanning the
  // segment list so we can cap to actual segment count.
  if (!ensureScanned())
  {
    return false;
  }

  const std::size_t n_segs = _segments.size();
  if (n_segs == 0)
  {
    for (auto* agg : aggregators)
    {
      if (agg != nullptr)
      {
        agg->finalize();
      }
    }
    return true;
  }

  // Resolve effective worker count.
  std::size_t max_threads;
  // Ask the panel before deciding anything. An aggregator that needs the whole
  // tape in order cannot be cloned into workers; auto silently becomes a
  // single thread, an explicit request is refused here rather than from
  // inside a worker halfway through the walk.
  bool panel_supports_parallel = true;
  for (auto* agg : aggregators)
  {
    if (agg != nullptr && !agg->supportsParallel())
    {
      panel_supports_parallel = false;
      break;
    }
  }

  if (n_threads == 0)
  {
    const unsigned hc = std::thread::hardware_concurrency();
    max_threads = hc > 0 ? static_cast<std::size_t>(hc) : std::size_t{1};
  }
  else
  {
    max_threads = n_threads;
  }

  if (!panel_supports_parallel)
  {
    if (n_threads > 1)
    {
      throw lrvx::LrvxError(
          "E_DATA_003",
          "run(aggregators, n_threads=" + std::to_string(n_threads) +
              "): the panel contains an aggregator that cannot run on partitioned "
              "workers (book reconstruction is order-dependent across the whole "
              "tape). Call run(aggregators, 1).");
    }
    max_threads = 1;
  }
  if (max_threads <= 1)
  {
    // Auto resolved to 1 (single-core box or hardware_concurrency=0):
    // single-thread path.
    const bool walked = streamForEach(
        [&aggregators](const ReplayEvent& ev)
        {
          for (auto* agg : aggregators)
          {
            if (agg != nullptr)
            {
              agg->onEvent(ev);
            }
          }
          return true;
        });
    for (auto* agg : aggregators)
    {
      if (agg != nullptr)
      {
        agg->finalize();
      }
    }
    return walked;
  }

  // Intra-segment parallel path. For each segment we discover its
  // compressed-block offsets via a fast header-only walk, partition
  // those across at most `max_threads` workers, and merge each
  // segment's worker panels into the caller's originals before
  // moving to the next segment. This handles md_collector-style
  // captures with one huge active segment (segment-level partition
  // would leave workers idle while one thread chews the big file).
  //
  // Minimum-blocks-per-worker heuristic: don't bother spawning
  // threads for segments with fewer than 2× max_threads blocks —
  // the spawn / join / merge overhead dominates the work.
  constexpr std::size_t kMinBlocksPerWorker = 2;

  auto run_segment_single_thread = [&](const std::filesystem::path& path)
  {
    EventCallback cb = [&aggregators](const ReplayEvent& ev) -> bool
    {
      for (auto* agg : aggregators)
      {
        if (agg != nullptr)
        {
          agg->onEvent(ev);
        }
      }
      return true;
    };
    return streamSegmentWithStats(path, cb, _stats);
  };

  for (const auto& segment : _segments)
  {
    std::vector<BlockMeta> blocks;
    SegmentHeader seg_hdr;
    if (!scanBlockOffsetsForSegment(segment.path, seg_hdr, blocks))
    {
      return false;
    }

    const std::size_t workers_for_segment =
        std::min(max_threads, blocks.size() / kMinBlocksPerWorker);
    if (workers_for_segment <= 1)
    {
      // Too few blocks (or uncompressed segment) — single-thread it
      // through the master panel directly.
      if (!run_segment_single_thread(segment.path))
      {
        return false;
      }
      continue;
    }

    // Clone master panel per worker for THIS segment.
    std::vector<std::vector<std::unique_ptr<IAggregator>>> worker_panels(
        workers_for_segment);
    for (auto& panel : worker_panels)
    {
      panel.reserve(aggregators.size());
      for (auto* a : aggregators)
      {
        panel.push_back(a != nullptr ? a->cloneEmpty() : nullptr);
      }
    }

    std::vector<ReaderStats> local_stats(workers_for_segment);
    std::atomic<bool> ok{true};
    std::mutex error_mutex;
    std::exception_ptr first_error;

    const std::size_t n_blocks = blocks.size();
    const int64_t reorder_W = _config.reorder_window_ns;
    const bool strict_ordering = _config.strict_ordering;
    const std::filesystem::path& seg_path = segment.path;

    std::vector<std::thread> threads;
    threads.reserve(workers_for_segment);
    for (std::size_t ti = 0; ti < workers_for_segment; ++ti)
    {
      const std::size_t lo = (ti * n_blocks) / workers_for_segment;
      const std::size_t hi = ((ti + 1) * n_blocks) / workers_for_segment;
      threads.push_back(makeThread(
          "lrvx.replay.reader", [this, ti, lo, hi, &blocks, &worker_panels, &local_stats, &ok,
                                 &error_mutex, &first_error, &seg_path, reorder_W, strict_ordering]()
          {
            try
            {
              auto& panel = worker_panels[ti];
              EventCallback cb = [&panel](const ReplayEvent& ev) -> bool
              {
                for (auto& agg : panel)
                {
                  if (agg)
                  {
                    agg->onEvent(ev);
                  }
                }
                return true;
              };
              auto filter = [this](const ReplayEvent& ev)
              {
                return passesFilter(ev);
              };
              std::span<const BlockMeta> slice(blocks.data() + lo, hi - lo);
              if (!streamBlockRangeImpl(seg_path, slice, cb, local_stats[ti],
                                        reorder_W, strict_ordering, filter))
              {
                ok.store(false, std::memory_order_relaxed);
              }
            }
            catch (...)
            {
              std::lock_guard<std::mutex> g(error_mutex);
              if (!first_error)
              {
                first_error = std::current_exception();
              }
              ok.store(false, std::memory_order_relaxed);
            } }));
    }
    for (auto& t : threads)
    {
      t.join();
    }
    if (first_error)
    {
      std::rethrow_exception(first_error);
    }
    if (!ok.load())
    {
      return false;
    }

    // Merge this segment's worker panels into the master panel.
    for (auto& panel : worker_panels)
    {
      for (std::size_t k = 0; k < aggregators.size(); ++k)
      {
        if (aggregators[k] != nullptr && panel[k] != nullptr)
        {
          aggregators[k]->merge(*panel[k]);
        }
      }
    }
    // Sum local stats from this segment's workers into master.
    for (const auto& s : local_stats)
    {
      _stats.files_read += s.files_read;
      _stats.events_read += s.events_read;
      _stats.trades_read += s.trades_read;
      _stats.book_updates_read += s.book_updates_read;
      _stats.bytes_read += s.bytes_read;
      _stats.crc_errors += s.crc_errors;
    }
  }

  for (auto* agg : aggregators)
  {
    if (agg != nullptr)
    {
      agg->finalize();
    }
  }
  return true;
}

bool BinaryLogReader::streamForEach(EventCallback callback)
{
  if (!ensureScanned())
  {
    return false;
  }

  _stream_watermark = std::numeric_limits<int64_t>::min();

  for (const auto& segment : _segments)
  {
    if (!readSegmentStreaming(segment.path, callback))
    {
      return false;
    }
  }

  return true;
}

bool BinaryLogReader::streamForEachFrom(int64_t start_ts_ns, EventCallback callback)
{
  if (!ensureScanned())
  {
    return false;
  }

  _stream_watermark = std::numeric_limits<int64_t>::min();

  auto it = std::lower_bound(_segments.begin(), _segments.end(), start_ts_ns,
                             [](const SegmentInfo& seg, int64_t ts)
                             { return seg.last_event_ns < ts; });

  for (; it != _segments.end(); ++it)
  {
    if (!readSegmentStreamingFrom(*it, start_ts_ns, callback))
    {
      return false;
    }
    // After first segment, deliver remaining segments from beginning.
    start_ts_ns = 0;
  }

  return true;
}

bool BinaryLogReader::readSegmentStreaming(const std::filesystem::path& path,
                                           EventCallback& callback)
{
  return streamSegmentWithStats(path, callback, _stats);
}

bool BinaryLogReader::streamSegmentWithStats(const std::filesystem::path& path,
                                             EventCallback& callback,
                                             ReaderStats& stats)
{
  BinaryLogIterator iter(path);
  if (!iter.isValid())
  {
    return false;
  }

  ++stats.files_read;

  // Sorted segments: stream straight through. The writer guaranteed
  // monotonicity at close, BinaryLogIterator preserves it (its
  // per-block sort is a no-op on the fast path for sorted blocks).
  //
  // The shared watermark is deliberately neither read nor advanced here. A
  // sorted segment carries its own ordering guarantee, so it is not the
  // reader's to judge against where the previous segment happened to end:
  // two captures of overlapping windows, or a symbol-partitioned recording,
  // are ordinary datasets, and dropping the head of the second one is losing
  // recorded events. Whether such a dataset should exist at all is the
  // validator's question (it reports overlapping segment ranges), not a
  // licence for the reader to discard frames.
  if (iter.header().isSorted())
  {
    ReplayEvent event;
    while (iter.next(event))
    {
      if (!passesFilter(event))
      {
        continue;
      }
      ++stats.events_read;
      event.type == EventType::Trade ? ++stats.trades_read : ++stats.book_updates_read;
      if (!callback(event))
      {
        return false;
      }
    }
    return true;
  }

  // Unsorted segments: bounded reorder buffer over already-block-sorted
  // events from the iterator. Per-block sort lifts intra-block jitter
  // (typically 1-2ms exchange-WS jitter, 95th-percentile in production
  // traces); the heap here handles cross-block inversions up to
  // reorder_window_ns (typically exchange-reconnect-induced batches of
  // older events appearing in a fresh block).
  auto passes = [this](const ReplayEvent& ev)
  {
    return passesFilter(ev);
  };
  ReorderBuffer<decltype(passes)> reorder(callback, stats, _config.reorder_window_ns,
                                          _config.strict_ordering, passes,
                                          _stream_watermark, path);

  ReplayEvent event;
  while (iter.next(event))
  {
    if (!reorder.offer(std::move(event), iter.lastEventOffset()))
    {
      return false;
    }
  }

  // Final drain — emit tail in sorted order.
  return reorder.drainAll();
}

bool BinaryLogReader::readSegmentStreamingFrom(const SegmentInfo& segment,
                                               int64_t start_ts_ns,
                                               EventCallback& callback)
{
  BinaryLogIterator iter(segment.path);
  if (!iter.isValid())
  {
    return false;
  }

  ++_stats.files_read;

  if (segment.has_index)
  {
    iter.loadIndex();
    if (iter.hasIndex())
    {
      iter.seekToTimestamp(start_ts_ns);
    }
  }

  // Sorted: straight stream with start_ts filter.
  if (iter.header().isSorted())
  {
    ReplayEvent event;
    while (iter.next(event))
    {
      if (event.timestamp_ns < start_ts_ns)
      {
        continue;
      }
      if (!passesFilter(event))
      {
        continue;
      }
      ++_stats.events_read;
      event.type == EventType::Trade ? ++_stats.trades_read : ++_stats.book_updates_read;
      if (!callback(event))
      {
        return false;
      }
    }
    return true;
  }

  // Unsorted: same bounded-reorder pattern as readSegmentStreaming,
  // with start_ts_ns applied after the heap re-orders.
  auto passes = [this, start_ts_ns](const ReplayEvent& ev)
  {
    return ev.timestamp_ns >= start_ts_ns && passesFilter(ev);
  };
  ReorderBuffer<decltype(passes)> reorder(callback, _stats, _config.reorder_window_ns,
                                          _config.strict_ordering, passes,
                                          _stream_watermark, segment.path);

  ReplayEvent event;
  while (iter.next(event))
  {
    if (!reorder.offer(std::move(event), iter.lastEventOffset()))
    {
      return false;
    }
  }

  return reorder.drainAll();
}

bool BinaryLogReader::forEachFrom(int64_t start_ts_ns, EventCallback callback)
{
  if (!ensureScanned())
  {
    return false;
  }

  // Find first segment that may contain events >= start_ts_ns
  auto it = std::lower_bound(_segments.begin(), _segments.end(), start_ts_ns,
                             [](const SegmentInfo& seg, int64_t ts)
                             {
                               return seg.last_event_ns < ts;
                             });

  for (; it != _segments.end(); ++it)
  {
    if (!readSegmentFrom(*it, start_ts_ns, callback))
    {
      return false;
    }
    // After first segment, read remaining segments from beginning
    start_ts_ns = 0;
  }

  return true;
}

std::optional<std::pair<int64_t, int64_t>> BinaryLogReader::timeRange() const
{
  if (!ensureScanned() || _segments.empty())
  {
    return std::nullopt;
  }

  int64_t min_ts = INT64_MAX;
  int64_t max_ts = INT64_MIN;

  for (const auto& segment : _segments)
  {
    if (segment.first_event_ns > 0)
    {
      min_ts = std::min(min_ts, segment.first_event_ns);
    }
    if (segment.last_event_ns > 0)
    {
      max_ts = std::max(max_ts, segment.last_event_ns);
    }
  }

  if (min_ts == INT64_MAX || max_ts == INT64_MIN)
  {
    return std::nullopt;
  }

  return std::make_pair(min_ts, max_ts);
}

ReaderStats BinaryLogReader::stats() const { return _stats; }

std::vector<std::filesystem::path> BinaryLogReader::segmentFiles() const
{
  std::vector<std::filesystem::path> paths;
  paths.reserve(_segments.size());
  for (const auto& seg : _segments)
  {
    paths.push_back(seg.path);
  }
  return paths;
}

const std::vector<SegmentInfo>& BinaryLogReader::segments() const
{
  // Without this the accessor answers "no segments" until some other call
  // happens to scan, which is indistinguishable from an empty directory.
  ensureScanned();
  return _segments;
}

DatasetSummary BinaryLogReader::inspect(const std::filesystem::path& data_dir)
{
  DatasetSummary summary;
  summary.data_dir = data_dir;

  namespace fs = std::filesystem;

  if (!fs::exists(data_dir))
  {
    return summary;
  }

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : fs::directory_iterator(data_dir))
  {
    if (entry.is_regular_file() && isSegmentExtension(entry.path().extension().string()))
    {
      paths.push_back(entry.path());
    }
  }

  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths)
  {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
    {
      continue;
    }

    SegmentHeader header;
    if (std::fread(&header, sizeof(header), 1, f) == 1 && header.isValid())
    {
      ++summary.segment_count;

      int64_t first_ns = header.first_event_ns;
      int64_t last_ns = header.last_event_ns;
      uint32_t ev_count = header.event_count;

      if (ev_count == 0)
      {
        if (header.isCompressed())
        {
          recoverCompressedSegmentMeta(f, header, ev_count, first_ns, last_ns);
        }
        else
        {
          recoverUncompressedSegmentMeta(f, header, ev_count, first_ns, last_ns);
        }
      }

      summary.total_events += ev_count;

      // Get file size
      std::fseek(f, 0, SEEK_END);
      summary.total_bytes += static_cast<uint64_t>(std::ftell(f));

      // Update time range
      if (first_ns > 0 && (summary.first_event_ns == 0 || first_ns < summary.first_event_ns))
      {
        summary.first_event_ns = first_ns;
      }
      if (last_ns > summary.last_event_ns)
      {
        summary.last_event_ns = last_ns;
      }

      // Index status
      if (header.hasIndex())
      {
        ++summary.segments_with_index;
      }
      else
      {
        ++summary.segments_without_index;
      }
    }
    std::fclose(f);
  }

  return summary;
}

DatasetSummary BinaryLogReader::inspectWithSymbols(const std::filesystem::path& data_dir)
{
  // First get basic info
  DatasetSummary summary = inspect(data_dir);

  // Then scan all events to collect symbols
  ReaderConfig config{.data_dir = data_dir, .verify_crc = false};  // Skip CRC for speed
  BinaryLogReader reader(std::move(config));

  reader.forEach([&summary](const ReplayEvent& event)
                 {
    uint32_t symbol_id = event.symbolId();
    summary.symbols.insert(symbol_id);
    return true; });

  return summary;
}

DatasetSummary BinaryLogReader::summary()
{
  ensureScanned();

  DatasetSummary result;
  result.data_dir = _config.data_dir;

  for (const auto& segment : _segments)
  {
    ++result.segment_count;
    result.total_events += segment.event_count;

    // Update time range. A segment whose first timestamp is still unknown
    // (zero) would otherwise pull the dataset's start back to 1970 and make
    // every duration computed off the summary meaningless.
    if (segment.first_event_ns > 0 &&
        (result.first_event_ns == 0 || segment.first_event_ns < result.first_event_ns))
    {
      result.first_event_ns = segment.first_event_ns;
    }
    if (segment.last_event_ns > result.last_event_ns)
    {
      result.last_event_ns = segment.last_event_ns;
    }

    // Index status
    if (segment.has_index)
    {
      ++result.segments_with_index;
    }
    else
    {
      ++result.segments_without_index;
    }

    // Get file size
    std::error_code ec;
    auto file_size = std::filesystem::file_size(segment.path, ec);
    if (!ec)
    {
      result.total_bytes += file_size;
    }
  }

  return result;
}

uint64_t BinaryLogReader::count()
{
  ensureScanned();

  uint64_t total = 0;
  for (const auto& segment : _segments)
  {
    total += segment.event_count;
  }
  return total;
}

std::set<uint32_t> BinaryLogReader::availableSymbols()
{
  std::set<uint32_t> symbols;

  forEach([&symbols](const ReplayEvent& event)
          {
    uint32_t symbol_id = event.symbolId();
    symbols.insert(symbol_id);
    return true; });

  return symbols;
}

BinaryLogIterator::BinaryLogIterator(const std::filesystem::path& segment_path)
{
  _file = std::fopen(segment_path.string().c_str(), "rb");
  if (!_file)
  {
    return;
  }

  // Read and validate segment header
  if (std::fread(&_header, sizeof(_header), 1, _file) != 1 || !_header.isValid())
  {
    std::fclose(_file);
    _file = nullptr;
    return;
  }

  _payload_buffer.reserve(64 * 1024);  // 64KB initial buffer
}

BinaryLogIterator::~BinaryLogIterator()
{
  if (_file)
  {
    std::fclose(_file);
  }
}

bool BinaryLogIterator::next(ReplayEvent& out)
{
  if (!_file)
  {
    return false;
  }

  if (isCompressed())
  {
    return nextCompressed(out);
  }
  else
  {
    return nextUncompressed(out);
  }
}

bool BinaryLogIterator::nextUncompressed(ReplayEvent& out)
{
  // A frame the reader cannot parse used to end the read, which is
  // indistinguishable from the end of the file. The format promises that
  // record types are additive and that an older reader walks past an unknown
  // one via FrameHeader.size, so that is what happens here; a CRC mismatch is
  // counted and skipped the same way rather than silently truncating the tape.
  for (;;)
  {
    // Check if we've hit the index (stop reading events)
    if (_header.hasIndex())
    {
      long current_pos = std::ftell(_file);
      if (current_pos >= static_cast<long>(_header.index_offset))
      {
        return false;  // Reached index, no more events
      }
    }

    const long frame_pos = std::ftell(_file);

    // Read frame header
    FrameHeader frame;
    if (std::fread(&frame, sizeof(frame), 1, _file) != 1)
    {
      return false;  // EOF or error
    }

    // A length this large is a corrupt header, not a record: there is no safe
    // way to find the next frame, so stop.
    if (frame.size > kMaxFrameBytes)
    {
      ++_crc_errors;
      return false;
    }

    // Read payload
    _payload_buffer.resize(frame.size);
    if (std::fread(_payload_buffer.data(), 1, frame.size, _file) != frame.size)
    {
      return false;  // Torn frame at the tail of a live segment
    }

    _last_event_offset = frame_pos >= 0 ? static_cast<uint64_t>(frame_pos) : 0;

    // Verify CRC
    uint32_t computed_crc = Crc32::compute(_payload_buffer);
    if (computed_crc != frame.crc32)
    {
      ++_crc_errors;
      continue;
    }

    if (!isKnownEventType(frame.type))
    {
      ++_unknown_frames_skipped;
      continue;
    }

    if (parseFrame(static_cast<EventType>(frame.type), _payload_buffer.data(), frame.size, out))
    {
      return true;
    }

    // Known type, unparseable payload: the frame length still tells us where
    // the next one starts, so keep going instead of calling it EOF.
    ++_crc_errors;
  }
}

bool BinaryLogIterator::nextCompressed(ReplayEvent& out)
{
  // If we have events remaining in current block, read from it.
  // In worker-mode (_stop_at_block_end=true), the iterator returns
  // false at the end of the current block instead of advancing —
  // the worker re-positions itself via seekToBlockOffset for the
  // next block in its assigned range.
  while (_block_events_remaining == 0)
  {
    if (_stop_at_block_end)
    {
      return false;
    }
    if (!loadNextBlock())
    {
      return false;  // No more blocks
    }
  }

  for (;;)
  {
    // Parse next frame from decompressed block data
    if (_block_offset + sizeof(FrameHeader) > _block_data.size())
    {
      return false;
    }

    FrameHeader frame;
    std::memcpy(&frame, _block_data.data() + _block_offset, sizeof(frame));
    _block_offset += sizeof(FrameHeader);

    // Sanity check
    if (frame.size > kMaxFrameBytes || _block_offset + frame.size > _block_data.size())
    {
      return false;
    }

    const std::byte* payload = _block_data.data() + _block_offset;
    _block_offset += frame.size;
    --_block_events_remaining;
    _last_event_offset = _block_file_offset;

    // Verify CRC
    if (Crc32::compute(payload, frame.size) != frame.crc32)
    {
      ++_crc_errors;
    }
    else if (!isKnownEventType(frame.type))
    {
      ++_unknown_frames_skipped;
    }
    else if (parseFrame(static_cast<EventType>(frame.type), payload, frame.size, out))
    {
      return true;
    }
    else
    {
      ++_crc_errors;
    }

    // Skipped a frame: fall through to the next one in this block, or to the
    // next block if this was the last.
    while (_block_events_remaining == 0)
    {
      if (_stop_at_block_end)
      {
        return false;
      }
      if (!loadNextBlock())
      {
        return false;
      }
    }
  }
}

bool BinaryLogIterator::loadNextBlock()
{
  // Check if we've hit the index (stop reading)
  if (_header.hasIndex())
  {
    long current_pos = std::ftell(_file);
    if (current_pos >= static_cast<long>(_header.index_offset))
    {
      return false;
    }
  }

  const long block_pos = std::ftell(_file);

  // Read compressed block header
  CompressedBlockHeader block_header;
  if (std::fread(&block_header, sizeof(block_header), 1, _file) != 1)
  {
    return false;  // EOF or error
  }
  _block_file_offset = block_pos >= 0 ? static_cast<uint64_t>(block_pos) : 0;

  if (!block_header.isValid())
  {
    return false;  // Invalid block
  }

  // Sanity check sizes — the 100 MB ceiling is a coarse upper bound for
  // sane blocks. Below it we also bound `compressed_size` by the bytes
  // remaining in the segment region, so a live-writing tape whose tail
  // header was flushed before its payload (or before any payload was
  // ever written) does not allocate up to 100 MB on garbage.
  if (block_header.compressed_size > 100 * 1024 * 1024 ||
      block_header.original_size > 100 * 1024 * 1024)
  {
    return false;
  }

  const long payload_pos = std::ftell(_file);
  long stop_at;
  if (_header.hasIndex() && _header.index_offset > 0)
  {
    stop_at = static_cast<long>(_header.index_offset);
  }
  else
  {
    if (std::fseek(_file, 0, SEEK_END) != 0)
    {
      return false;
    }
    stop_at = std::ftell(_file);
    if (stop_at < 0 || std::fseek(_file, payload_pos, SEEK_SET) != 0)
    {
      return false;
    }
  }
  if (payload_pos < 0 ||
      static_cast<long>(block_header.compressed_size) > stop_at - payload_pos)
  {
    return false;
  }

  // Read compressed data
  _payload_buffer.resize(block_header.compressed_size);
  if (std::fread(_payload_buffer.data(), 1, block_header.compressed_size, _file) !=
      block_header.compressed_size)
  {
    return false;
  }

  // Decompress
  _block_data.resize(block_header.original_size);
  size_t decompressed_size =
      Compressor::decompress(_header.compressionType(), _payload_buffer.data(),
                             block_header.compressed_size, _block_data.data(),
                             block_header.original_size);

  if (decompressed_size != block_header.original_size)
  {
    return false;  // Decompression failed
  }

  // Intra-block sort on Sorted=false segments. Lifts the 95th-percentile
  // of exchange-WS jitter (1-2ms) at the cost of one linear scan of the
  // block + a stable_sort over event-count refs only when the block is
  // genuinely unsorted. Sorted=true segments hit the monotonic fast path
  // (the writer already did this work on its side).
  if (!_header.isSorted())
  {
    sortBlockFramesInPlace(_block_data, block_header.event_count, _sort_scratch);
  }

  _block_offset = 0;
  _block_events_remaining = block_header.event_count;

  return true;
}

bool BinaryLogIterator::parseFrame(EventType type, const std::byte* data, size_t size,
                                   ReplayEvent& out)
{
  out.type = type;

  if (type == EventType::Trade)
  {
    if (size < sizeof(TradeRecord))
    {
      return false;
    }
    std::memcpy(&out.trade, data, sizeof(TradeRecord));
    out.timestamp_ns = out.trade.exchange_ts_ns;
    out.bids.clear();
    out.asks.clear();
    return true;
  }
  else if (type == EventType::OptionQuote)
  {
    if (size < sizeof(OptionQuoteRecord))
    {
      return false;
    }
    std::memcpy(&out.option_quote, data, sizeof(OptionQuoteRecord));
    out.timestamp_ns = out.option_quote.exchange_ts_ns;
    out.bids.clear();
    out.asks.clear();
    return true;
  }
  else if (type == EventType::PoolState)
  {
    if (size < sizeof(PoolStateRecordHeader))
    {
      return false;
    }
    std::memcpy(&out.pool_state_header, data, sizeof(PoolStateRecordHeader));
    out.timestamp_ns = out.pool_state_header.exchange_ts_ns;
    const size_t payload_len = out.pool_state_header.payload_len;
    if (size < sizeof(PoolStateRecordHeader) + payload_len)
    {
      return false;
    }
    const std::byte* payload = data + sizeof(PoolStateRecordHeader);
    out.pool_state_payload.assign(payload, payload + payload_len);
    out.bids.clear();
    out.asks.clear();
    return true;
  }
  else if (type == EventType::BookSnapshot || type == EventType::BookDelta ||
           type == EventType::BookTop)
  {
    if (size < sizeof(BookRecordHeader))
    {
      return false;
    }
    std::memcpy(&out.book_header, data, sizeof(BookRecordHeader));
    out.timestamp_ns = out.book_header.exchange_ts_ns;

    const size_t levels_offset = sizeof(BookRecordHeader);
    const size_t bid_bytes = out.book_header.bid_count * sizeof(BookLevel);
    const size_t ask_bytes = out.book_header.ask_count * sizeof(BookLevel);

    if (size < levels_offset + bid_bytes + ask_bytes)
    {
      return false;
    }

    out.bids.resize(out.book_header.bid_count);
    out.asks.resize(out.book_header.ask_count);

    if (out.book_header.bid_count > 0)
    {
      std::memcpy(out.bids.data(), data + levels_offset, bid_bytes);
    }
    if (out.book_header.ask_count > 0)
    {
      std::memcpy(out.asks.data(), data + levels_offset + bid_bytes, ask_bytes);
    }
    return true;
  }

  return false;  // Unknown type
}

bool BinaryLogIterator::seekToBlockOffset(uint64_t file_offset)
{
  if (!_file)
  {
    return false;
  }
  if (std::fseek(_file, static_cast<long>(file_offset), SEEK_SET) != 0)
  {
    return false;
  }
  // Discard current block state. Then immediately load the block at
  // the new file position so worker-mode `next()` calls can proceed
  // without falling into the stop-at-block-end short-circuit (which
  // fires whenever `_block_events_remaining == 0`).
  discardBlockState();
  return loadNextBlock();
}

bool BinaryLogIterator::loadIndex()
{
  if (!_file || !_header.hasIndex())
  {
    return false;
  }

  // Seek to index position
  if (std::fseek(_file, static_cast<long>(_header.index_offset), SEEK_SET) != 0)
  {
    return false;
  }

  // Read index header
  SegmentIndexHeader idx_header;
  if (std::fread(&idx_header, sizeof(idx_header), 1, _file) != 1)
  {
    // Seek back to data start
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  if (!idx_header.isValid() || idx_header.entry_count == 0)
  {
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  // Read index entries
  _index_entries.resize(idx_header.entry_count);
  if (std::fread(_index_entries.data(), sizeof(IndexEntry), idx_header.entry_count, _file) !=
      idx_header.entry_count)
  {
    _index_entries.clear();
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  // Verify CRC
  uint32_t computed_crc =
      Crc32::compute(_index_entries.data(), _index_entries.size() * sizeof(IndexEntry));
  if (computed_crc != idx_header.crc32)
  {
    _index_entries.clear();
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    return false;
  }

  // Seek back to data start
  std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
  return true;
}

bool BinaryLogIterator::seekToTimestamp(int64_t target_ts_ns)
{
  if (!_file)
  {
    return false;
  }
  // hasIndex() answers from the header, so callers reach here without having
  // called loadIndex(); load it on demand rather than refusing the seek.
  if (_index_entries.empty() && !loadIndex())
  {
    return false;
  }
  if (_index_entries.empty())
  {
    return false;
  }

  // Binary search for the largest entry with timestamp <= target
  auto it = std::upper_bound(_index_entries.begin(), _index_entries.end(), target_ts_ns,
                             [](int64_t ts, const IndexEntry& entry)
                             {
                               return ts < entry.timestamp_ns;
                             });

  // upper_bound gives first entry > target, we want the one before
  if (it == _index_entries.begin())
  {
    // All entries are > target, start from beginning
    std::fseek(_file, sizeof(SegmentHeader), SEEK_SET);
    discardBlockState();
    return true;
  }

  --it;  // Now points to largest entry <= target

  // Seek to that position
  if (std::fseek(_file, static_cast<long>(it->file_offset), SEEK_SET) != 0)
  {
    return false;
  }

  // A compressed segment keeps a decompressed block in memory. Moving the
  // file cursor without dropping it made the next reads replay the tail of
  // the block the iterator happened to be holding -- events from before the
  // requested timestamp, delivered after the seek.
  discardBlockState();

  return true;
}

void BinaryLogIterator::discardBlockState()
{
  _block_data.clear();
  _block_offset = 0;
  _block_events_remaining = 0;
}

}  // namespace lrvx::replay
