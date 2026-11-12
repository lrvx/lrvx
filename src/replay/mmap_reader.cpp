/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/readers/mmap_reader.h"
#include "segment_bounds.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lrvx::replay
{

MmapSegmentReader::MmapSegmentReader(const std::filesystem::path& segment_path)
{
#ifdef _WIN32
  _file_handle = CreateFileW(segment_path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (_file_handle == INVALID_HANDLE_VALUE)
  {
    _file_handle = nullptr;
    return;
  }

  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(_file_handle, &file_size))
  {
    CloseHandle(_file_handle);
    _file_handle = nullptr;
    return;
  }

  _mapped_size = static_cast<size_t>(file_size.QuadPart);
  if (_mapped_size < sizeof(SegmentHeader))
  {
    CloseHandle(_file_handle);
    _file_handle = nullptr;
    return;
  }

  _mapping_handle = CreateFileMappingW(_file_handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (_mapping_handle == nullptr)
  {
    CloseHandle(_file_handle);
    _file_handle = nullptr;
    return;
  }

  _mapped_data = MapViewOfFile(_mapping_handle, FILE_MAP_READ, 0, 0, 0);
  if (_mapped_data == nullptr)
  {
    CloseHandle(_mapping_handle);
    _mapping_handle = nullptr;
    CloseHandle(_file_handle);
    _file_handle = nullptr;
    return;
  }
#else
  _fd = ::open(segment_path.string().c_str(), O_RDONLY);
  if (_fd < 0)
  {
    return;
  }

  struct stat st;
  if (::fstat(_fd, &st) < 0)
  {
    ::close(_fd);
    _fd = -1;
    return;
  }

  _mapped_size = static_cast<size_t>(st.st_size);
  if (_mapped_size < sizeof(SegmentHeader))
  {
    ::close(_fd);
    _fd = -1;
    return;
  }

  _mapped_data = ::mmap(nullptr, _mapped_size, PROT_READ, MAP_PRIVATE, _fd, 0);
  if (_mapped_data == MAP_FAILED)
  {
    _mapped_data = nullptr;
    ::close(_fd);
    _fd = -1;
    return;
  }

  ::madvise(_mapped_data, _mapped_size, MADV_SEQUENTIAL);
#endif

  _header = reinterpret_cast<const SegmentHeader*>(_mapped_data);
  if (!_header->isValid())
  {
    cleanup();
    return;
  }

  _data_start = reinterpret_cast<const std::byte*>(_mapped_data) + sizeof(SegmentHeader);

  _data_end = reinterpret_cast<const std::byte*>(_mapped_data) + _mapped_size;
  if (_header->hasIndex())
  {
    if (_header->index_offset > _mapped_size)
    {
      _data_short = true;
    }
    else
    {
      _data_end = reinterpret_cast<const std::byte*>(_mapped_data) +
                  std::max<uint64_t>(_header->index_offset, sizeof(SegmentHeader));
    }
  }

  _current = _data_start;
}

void MmapSegmentReader::cleanup()
{
#ifdef _WIN32
  if (_mapped_data)
  {
    UnmapViewOfFile(_mapped_data);
    _mapped_data = nullptr;
  }
  if (_mapping_handle)
  {
    CloseHandle(_mapping_handle);
    _mapping_handle = nullptr;
  }
  if (_file_handle)
  {
    CloseHandle(_file_handle);
    _file_handle = nullptr;
  }
#else
  if (_mapped_data)
  {
    ::munmap(_mapped_data, _mapped_size);
    _mapped_data = nullptr;
  }
  if (_fd >= 0)
  {
    ::close(_fd);
    _fd = -1;
  }
#endif
  _header = nullptr;
  _data_start = nullptr;
  _data_end = nullptr;
  _current = nullptr;
  _mapped_size = 0;
}

MmapSegmentReader::~MmapSegmentReader()
{
  cleanup();
}

MmapSegmentReader::MmapSegmentReader(MmapSegmentReader&& other) noexcept
    : _mapped_data(other._mapped_data),
      _mapped_size(other._mapped_size),
#ifdef _WIN32
      _file_handle(other._file_handle),
      _mapping_handle(other._mapping_handle),
#else
      _fd(other._fd),
#endif
      _header(other._header),
      _data_start(other._data_start),
      _data_end(other._data_end),
      _current(other._current),
      _index_entries(std::move(other._index_entries)),
      _index_loaded(other._index_loaded),
      _data_short(other._data_short),
      _verify_crc(other._verify_crc),
      _crc_errors(other._crc_errors),
      _truncated(other._truncated)
{
  other._mapped_data = nullptr;
#ifdef _WIN32
  other._file_handle = nullptr;
  other._mapping_handle = nullptr;
#else
  other._fd = -1;
#endif
}

MmapSegmentReader& MmapSegmentReader::operator=(MmapSegmentReader&& other) noexcept
{
  if (this != &other)
  {
    cleanup();

    _mapped_data = other._mapped_data;
    _mapped_size = other._mapped_size;
#ifdef _WIN32
    _file_handle = other._file_handle;
    _mapping_handle = other._mapping_handle;
#else
    _fd = other._fd;
#endif
    _header = other._header;
    _data_start = other._data_start;
    _data_end = other._data_end;
    _current = other._current;
    _index_entries = std::move(other._index_entries);
    _index_loaded = other._index_loaded;
    _data_short = other._data_short;
    _verify_crc = other._verify_crc;
    _crc_errors = other._crc_errors;
    _truncated = other._truncated;

    other._mapped_data = nullptr;
#ifdef _WIN32
    other._file_handle = nullptr;
    other._mapping_handle = nullptr;
#else
    other._fd = -1;
#endif
  }
  return *this;
}

bool MmapSegmentReader::isValid() const
{
  return _mapped_data != nullptr && _header != nullptr;
}

bool MmapSegmentReader::isCompressed() const
{
  return _header && _header->isCompressed();
}

bool MmapSegmentReader::hasIndex() const
{
  return _header && _header->hasIndex();
}

const SegmentHeader& MmapSegmentReader::header() const
{
  static SegmentHeader empty{};
  return _header ? *_header : empty;
}

const std::byte* MmapSegmentReader::data() const
{
  return _data_start;
}

size_t MmapSegmentReader::dataSize() const
{
  return _data_end - _data_start;
}

size_t MmapSegmentReader::totalSize() const
{
  return _mapped_size;
}

void MmapSegmentReader::reset()
{
  _current = _data_start;
  _crc_errors = 0;
  _truncated = false;
}

size_t MmapSegmentReader::position() const
{
  return _current - _data_start;
}

const FrameHeader* MmapSegmentReader::currentFrame() const
{
  if (_current + sizeof(FrameHeader) > _data_end)
  {
    return nullptr;
  }
  return reinterpret_cast<const FrameHeader*>(_current);
}

bool MmapSegmentReader::advanceFrame()
{
  const auto* frame = currentFrame();
  if (!frame)
  {
    return false;
  }

  size_t frame_size = sizeof(FrameHeader) + frame->size;
  if (_current + frame_size > _data_end)
  {
    return false;
  }

  _current += frame_size;
  return true;
}

const FrameHeader* MmapSegmentReader::nextCheckedFrame()
{
  // The readers' ceiling: a frame above 10 MiB is a damaged length field.
  constexpr uint32_t kMaxFrameBytes = 10u * 1024u * 1024u;
  // Without an index the data runs to the end of the file, whose tail may
  // still be being written; with one, data that stops short has lost bytes.
  const bool live_tail = !hasIndex();

  while (!_truncated && _current < _data_end)
  {
    const size_t left = static_cast<size_t>(_data_end - _current);
    if (left < sizeof(FrameHeader))
    {
      _truncated = !live_tail;
      return nullptr;
    }
    FrameHeader frame;
    std::memcpy(&frame, _current, sizeof(frame));
    if (frame.size > kMaxFrameBytes)
    {
      _truncated = true;
      return nullptr;
    }
    if (left - sizeof(FrameHeader) < frame.size)
    {
      _truncated = !live_tail;
      return nullptr;
    }

    const auto* at = reinterpret_cast<const FrameHeader*>(_current);
    const std::byte* payload = _current + sizeof(FrameHeader);
    _current += sizeof(FrameHeader) + frame.size;

    if (_verify_crc && Crc32::compute(payload, frame.size) != frame.crc32)
    {
      ++_crc_errors;
      continue;
    }
    return at;
  }
  if (_data_short)
  {
    _truncated = true;
  }
  return nullptr;
}

bool MmapSegmentReader::next(ReplayEvent& out)
{
  if (isCompressed())
  {
    return false;
  }

  // As BinaryLogIterator::nextUncompressed: a frame of a type this build does
  // not know is walked past, and one too short for its record is counted and
  // walked past, rather than ending the segment.
  while (const FrameHeader* at = nextCheckedFrame())
  {
    FrameHeader frame;
    std::memcpy(&frame, at, sizeof(frame));
    const std::byte* payload = reinterpret_cast<const std::byte*>(at) + sizeof(FrameHeader);

    if (frameRecordSize(frame.type) == 0)
    {
      continue;
    }
    // Checks the payload against its type's fixed record as well.
    if (!frameTimestamp(frame.type, payload, frame.size, out.timestamp_ns))
    {
      ++_crc_errors;
      continue;
    }
    out.type = static_cast<EventType>(frame.type);

    if (frame.type == static_cast<uint8_t>(EventType::Trade))
    {
      std::memcpy(&out.trade, payload, sizeof(TradeRecord));
      out.bids.clear();
      out.asks.clear();
      return true;
    }
    if (frame.type == static_cast<uint8_t>(EventType::OptionQuote))
    {
      std::memcpy(&out.option_quote, payload, sizeof(OptionQuoteRecord));
      out.bids.clear();
      out.asks.clear();
      return true;
    }
    if (frame.type == static_cast<uint8_t>(EventType::PoolState))
    {
      std::memcpy(&out.pool_state_header, payload, sizeof(PoolStateRecordHeader));
      const size_t payload_len = out.pool_state_header.payload_len;
      if (frame.size < sizeof(PoolStateRecordHeader) + payload_len)
      {
        ++_crc_errors;
        continue;
      }
      const std::byte* pool_payload = payload + sizeof(PoolStateRecordHeader);
      out.pool_state_payload.assign(pool_payload, pool_payload + payload_len);
      out.bids.clear();
      out.asks.clear();
      return true;
    }

    // BookSnapshot, BookDelta, BookTop.
    std::memcpy(&out.book_header, payload, sizeof(BookRecordHeader));
    const size_t bid_n = out.book_header.bid_count;
    const size_t ask_n = out.book_header.ask_count;
    if (frame.size < sizeof(BookRecordHeader) + (bid_n + ask_n) * sizeof(BookLevel))
    {
      ++_crc_errors;
      continue;
    }
    out.bids.resize(bid_n);
    out.asks.resize(ask_n);
    const std::byte* levels = payload + sizeof(BookRecordHeader);
    if (bid_n > 0)
    {
      std::memcpy(out.bids.data(), levels, bid_n * sizeof(BookLevel));
    }
    if (ask_n > 0)
    {
      std::memcpy(out.asks.data(), levels + bid_n * sizeof(BookLevel), ask_n * sizeof(BookLevel));
    }
    return true;
  }
  return false;
}

bool MmapSegmentReader::seekToTimestamp(int64_t target_ts_ns)
{
  if (!isValid())
  {
    return false;
  }
  // Same rule as BinaryLogIterator::seekToTimestamp: the last index entry
  // strictly below the target, and only in a Sorted segment; otherwise the
  // reader goes back to the first event and the caller walks the segment.
  std::optional<detail::IndexWindow> window;
  if (_header->isSorted() && hasIndex() && loadIndex())
  {
    window = detail::indexWindow(_index_entries, *_header, target_ts_ns,
                                 std::numeric_limits<int64_t>::max());
  }
  if (!window || window->start - sizeof(SegmentHeader) >= dataSize())
  {
    reset();
    return false;
  }
  _current = _data_start + (window->start - sizeof(SegmentHeader));
  return true;
}

bool MmapSegmentReader::loadIndex()
{
  if (_index_loaded)
  {
    return !_index_entries.empty();
  }

  _index_loaded = true;

  if (!hasIndex() || _header->index_offset == 0)
  {
    return false;
  }

  const auto* index_start =
      reinterpret_cast<const std::byte*>(_mapped_data) + _header->index_offset;
  const auto* file_end = reinterpret_cast<const std::byte*>(_mapped_data) + _mapped_size;

  if (index_start + sizeof(SegmentIndexHeader) > file_end)
  {
    return false;
  }

  // Copy to aligned temporary to avoid UB from misaligned access
  SegmentIndexHeader idx_header;
  std::memcpy(&idx_header, index_start, sizeof(SegmentIndexHeader));
  if (idx_header.magic != kIndexMagic)
  {
    return false;
  }

  const auto* entries_start = index_start + sizeof(SegmentIndexHeader);
  size_t entries_size = idx_header.entry_count * sizeof(IndexEntry);

  if (entries_start + entries_size > file_end)
  {
    return false;
  }

  // Copy entries to properly aligned vector
  _index_entries.resize(idx_header.entry_count);
  std::memcpy(_index_entries.data(), entries_start, entries_size);
  return true;
}

const std::vector<IndexEntry>& MmapSegmentReader::indexEntries() const
{
  return _index_entries;
}

MmapReader::MmapReader(const Config& config) : _config(config)
{
  ReaderConfig reader_config{
      .data_dir = _config.data_dir,
      .from_ns = _config.from_ns,
      .to_ns = _config.to_ns,
      .symbols = _config.symbols,
  };

  // The order BinaryLogReader visits segments in: by proven first event where
  // the segment proves one, by file name otherwise.
  BinaryLogReader scanner(reader_config);
  _segments = scanner.segmentsInReadOrder();

  // One reader per segment, valid or not, so _readers[i] maps _segments[i]:
  // a segment that cannot be mapped (removed or replaced after the scan) is
  // skipped where it stands instead of shifting every later segment's
  // metadata onto the wrong file.
  _readers.reserve(_segments.size());
  for (const auto& seg : _segments)
  {
    auto reader = std::make_unique<MmapSegmentReader>(seg.path);
    reader->setVerifyCrc(_config.verify_crc);
    if (reader->isValid())
    {
      _stats.segments_mapped++;
      _stats.bytes_mapped += reader->totalSize();

      if (_config.preload_index && reader->hasIndex())
      {
        reader->loadIndex();
      }

      if (_config.prefault_pages)
      {
        volatile char dummy = 0;
        const char* data = reinterpret_cast<const char*>(reader->data());
        size_t size = reader->dataSize();
        for (size_t i = 0; i < size; i += 4096)
        {
          dummy += data[i];
        }
        (void)dummy;
      }
    }
    _readers.push_back(std::move(reader));
  }
}

MmapReader::~MmapReader() = default;

namespace
{
// Adds a segment read's CRC and truncation counters to the reader's stats on
// every way out of the read, a callback that stops the walk included.
class SegmentTally
{
 public:
  SegmentTally(const MmapSegmentReader& seg, MmapReaderStats& stats) : _seg(seg), _stats(stats) {}
  ~SegmentTally()
  {
    _stats.crc_errors += _seg.crcErrors();
    _stats.segments_truncated += _seg.truncated() ? 1 : 0;
  }
  SegmentTally(const SegmentTally&) = delete;
  SegmentTally& operator=(const SegmentTally&) = delete;

 private:
  const MmapSegmentReader& _seg;
  MmapReaderStats& _stats;
};
}  // namespace

bool MmapReader::passesFilter(int64_t timestamp, uint32_t symbol_id) const
{
  if (_config.from_ns.has_value() && timestamp < *_config.from_ns)
  {
    return false;
  }
  if (_config.to_ns.has_value() && timestamp > *_config.to_ns)
  {
    return false;
  }
  if (!_config.symbols.empty() && _config.symbols.find(symbol_id) == _config.symbols.end())
  {
    return false;
  }
  return true;
}

uint64_t MmapReader::forEach(EventCallback callback)
{
  uint64_t count = 0;

  for (auto& reader : _readers)
  {
    if (!reader->isValid() || reader->isCompressed())
    {
      continue;
    }

    reader->reset();
    SegmentTally tally(*reader, _stats);
    ReplayEvent event;

    while (reader->next(event))
    {
      uint32_t symbol_id = event.symbolId();

      if (!passesFilter(event.timestamp_ns, symbol_id))
      {
        continue;
      }

      if (!callback(event))
      {
        return count;
      }
      ++count;
    }
  }

  _stats.events_read = count;
  return count;
}

uint64_t MmapReader::forEachFrom(int64_t start_ts_ns, EventCallback callback)
{
  uint64_t count = 0;

  for (size_t i = 0; i < _readers.size(); ++i)
  {
    auto& reader = _readers[i];
    if (!reader->isValid() || reader->isCompressed())
    {
      continue;
    }

    // Passed over only when its proven maximum lies below the start; the
    // header's last_event_ns bounds nothing on a segment without Sorted.
    if (detail::provenOutside(_segments[i], start_ts_ns, std::numeric_limits<int64_t>::max()))
    {
      continue;
    }

    // Leaves the reader at the first event when the index cannot position
    // the start (no index, or no Sorted flag).
    reader->reset();
    reader->seekToTimestamp(start_ts_ns);
    SegmentTally tally(*reader, _stats);

    ReplayEvent event;
    while (reader->next(event))
    {
      if (event.timestamp_ns < start_ts_ns)
      {
        continue;
      }

      uint32_t symbol_id = event.symbolId();

      if (!passesFilter(event.timestamp_ns, symbol_id))
      {
        continue;
      }

      if (!callback(event))
      {
        return count;
      }
      ++count;
    }
  }

  _stats.events_read = count;
  return count;
}

uint64_t MmapReader::forEachRawTrade(RawTradeCallback callback)
{
  uint64_t count = 0;

  for (auto& reader : _readers)
  {
    if (!reader->isValid() || reader->isCompressed())
    {
      continue;
    }

    // The frame walk next() takes, so a trade that fails its CRC is skipped
    // and counted here too; only trade frames are decoded.
    reader->reset();
    SegmentTally tally(*reader, _stats);
    while (const FrameHeader* at = reader->nextCheckedFrame())
    {
      FrameHeader frame;
      std::memcpy(&frame, at, sizeof(frame));
      if (frame.type != static_cast<uint8_t>(EventType::Trade))
      {
        continue;
      }
      if (frame.size < sizeof(TradeRecord))
      {
        ++reader->_crc_errors;
        continue;
      }
      // Copy to an aligned record: the payload follows a 12-byte FrameHeader.
      TradeRecord trade;
      std::memcpy(&trade, reinterpret_cast<const std::byte*>(at) + sizeof(FrameHeader),
                  sizeof(TradeRecord));
      if (passesFilter(trade.exchange_ts_ns, trade.symbol_id))
      {
        if (!callback(&trade))
        {
          return count;
        }
        ++count;
      }
    }
  }

  _stats.events_read = count;
  return count;
}

MmapReaderStats MmapReader::stats() const
{
  return _stats;
}

uint64_t MmapReader::totalEvents() const
{
  uint64_t total = 0;
  for (const auto& seg : _segments)
  {
    total += seg.event_count;
  }
  return total;
}

}  // namespace lrvx::replay
