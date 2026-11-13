/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/run/trace_reader.h"

#include "lrvx/replay/binary_format_v1.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace lrvx::run
{

namespace
{

// event_count is a 32-bit field read straight out of the file, so reserving
// against it hands an attacker-controlled allocation of up to four billion
// records. No segment can hold more records than its own byte count allows,
// so cap the reservation at that.
size_t safeReserveCount(uint32_t declared, size_t segmentBytes, size_t recordBytes)
{
  const size_t frameBytes = sizeof(lrvx::replay::FrameHeader) + recordBytes;
  const size_t possible = frameBytes > 0 ? segmentBytes / frameBytes : 0;
  return std::min(static_cast<size_t>(declared), possible);
}

// Minimal JSON pull parser. Just enough to read the manifest produced by the
// recorder. We avoid pulling a JSON library for the engine target so the run
// directory can be parsed in environments where one is not available.
class MiniJson
{
 public:
  explicit MiniJson(std::string_view src) : _src(src), _pos(0) {}

  void skipWs()
  {
    while (_pos < _src.size() && (_src[_pos] == ' ' || _src[_pos] == '\t' ||
                                  _src[_pos] == '\n' || _src[_pos] == '\r' ||
                                  _src[_pos] == ','))
    {
      ++_pos;
    }
  }

  bool match(char c)
  {
    skipWs();
    if (_pos < _src.size() && _src[_pos] == c)
    {
      ++_pos;
      return true;
    }
    return false;
  }

  std::string parseString()
  {
    skipWs();
    if (_pos >= _src.size() || _src[_pos] != '"')
    {
      throw std::runtime_error("manifest parse: expected string");
    }
    ++_pos;
    std::string out;
    while (_pos < _src.size() && _src[_pos] != '"')
    {
      if (_src[_pos] == '\\' && _pos + 1 < _src.size())
      {
        char nxt = _src[_pos + 1];
        switch (nxt)
        {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'u':
          {
            if (_pos + 5 >= _src.size())
            {
              throw std::runtime_error("manifest parse: truncated \\u escape");
            }
            std::string hex(_src.substr(_pos + 2, 4));
            unsigned code = std::stoul(hex, nullptr, 16);
            if (code < 0x80)
            {
              out += static_cast<char>(code);
            }
            else
            {
              // Manifest does not use non-ASCII; degrade safely.
              out += '?';
            }
            _pos += 4;
            break;
          }
          default:
            out += nxt;
            break;
        }
        _pos += 2;
        continue;
      }
      out += _src[_pos];
      ++_pos;
    }
    if (_pos >= _src.size())
    {
      throw std::runtime_error("manifest parse: unterminated string");
    }
    ++_pos;
    return out;
  }

  long long parseNumber()
  {
    skipWs();
    size_t start = _pos;
    if (_pos < _src.size() && (_src[_pos] == '-' || _src[_pos] == '+'))
    {
      ++_pos;
    }
    while (_pos < _src.size() && (std::isdigit(static_cast<unsigned char>(_src[_pos])) ||
                                  _src[_pos] == '.'))
    {
      ++_pos;
    }
    return std::stoll(std::string(_src.substr(start, _pos - start)));
  }

  void skipValue()
  {
    skipWs();
    if (_pos >= _src.size())
    {
      return;
    }
    char c = _src[_pos];
    if (c == '"')
    {
      parseString();
    }
    else if (c == '{' || c == '[')
    {
      char open = c;
      char close = (c == '{') ? '}' : ']';
      int depth = 1;
      ++_pos;
      while (_pos < _src.size() && depth > 0)
      {
        if (_src[_pos] == '"')
        {
          parseString();
          continue;
        }
        if (_src[_pos] == open)
        {
          ++depth;
        }
        else if (_src[_pos] == close)
        {
          --depth;
        }
        ++_pos;
      }
    }
    else
    {
      while (_pos < _src.size() && _src[_pos] != ',' && _src[_pos] != '}' &&
             _src[_pos] != ']')
      {
        ++_pos;
      }
    }
  }

 private:
  std::string_view _src;
  size_t _pos;
};

RecordKind kindFromString(const std::string& s)
{
  if (s == "signals")
  {
    return RecordKind::Signal;
  }
  if (s == "orders")
  {
    return RecordKind::OrderEvent;
  }
  if (s == "fills")
  {
    return RecordKind::Fill;
  }
  return RecordKind::Unknown;
}

}  // namespace

TraceReader::TraceReader(const std::string& path) : _root(path)
{
  loadManifest();
}

TraceReader::~TraceReader() = default;

void TraceReader::loadManifest()
{
  std::filesystem::path manifest_path = std::filesystem::path(_root) / "manifest.json";
  std::ifstream in(manifest_path);
  if (!in)
  {
    throw std::runtime_error("trace reader: missing manifest at " + manifest_path.string());
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string src = buf.str();
  MiniJson p(src);
  if (!p.match('{'))
  {
    throw std::runtime_error("trace reader: manifest must be a JSON object");
  }
  while (true)
  {
    p.skipWs();
    if (p.match('}'))
    {
      break;
    }
    std::string key = p.parseString();
    if (!p.match(':'))
    {
      throw std::runtime_error("trace reader: expected ':' after key " + key);
    }
    if (key == "schema_version")
    {
      _manifest.schema_version = static_cast<uint32_t>(p.parseNumber());
    }
    else if (key == "format_version")
    {
      _manifest.format_version = static_cast<uint32_t>(p.parseNumber());
    }
    else if (key == "strategy_id")
    {
      _manifest.strategy_id = p.parseString();
    }
    else if (key == "strategy_hash")
    {
      _manifest.strategy_hash = p.parseString();
    }
    else if (key == "run_started_ns")
    {
      _manifest.run_started_ns = p.parseNumber();
    }
    else if (key == "run_ended_ns")
    {
      _manifest.run_ended_ns = p.parseNumber();
    }
    else if (key == "tape_refs")
    {
      if (!p.match('['))
      {
        throw std::runtime_error("trace reader: tape_refs must be array");
      }
      while (true)
      {
        p.skipWs();
        if (p.match(']'))
        {
          break;
        }
        if (!p.match('{'))
        {
          throw std::runtime_error("trace reader: tape ref must be object");
        }
        TapeRef ref;
        while (true)
        {
          p.skipWs();
          if (p.match('}'))
          {
            break;
          }
          std::string k = p.parseString();
          if (!p.match(':'))
          {
            throw std::runtime_error("trace reader: expected ':' in tape ref");
          }
          if (k == "path")
          {
            ref.path = p.parseString();
          }
          else if (k == "content_hash")
          {
            ref.content_hash = p.parseString();
          }
          else if (k == "first_event_ns")
          {
            ref.first_event_ns = p.parseNumber();
          }
          else if (k == "last_event_ns")
          {
            ref.last_event_ns = p.parseNumber();
          }
          else
          {
            p.skipValue();
          }
        }
        _manifest.tape_refs.push_back(std::move(ref));
      }
    }
    else if (key == "segments")
    {
      if (!p.match('['))
      {
        throw std::runtime_error("trace reader: segments must be array");
      }
      while (true)
      {
        p.skipWs();
        if (p.match(']'))
        {
          break;
        }
        if (!p.match('{'))
        {
          throw std::runtime_error("trace reader: segment must be object");
        }
        TraceManifest::Segment seg;
        while (true)
        {
          p.skipWs();
          if (p.match('}'))
          {
            break;
          }
          std::string k = p.parseString();
          if (!p.match(':'))
          {
            throw std::runtime_error("trace reader: expected ':' in segment");
          }
          if (k == "name")
          {
            seg.name = p.parseString();
          }
          else if (k == "record_kind")
          {
            seg.record_kind = kindFromString(p.parseString());
          }
          else if (k == "size_bytes")
          {
            seg.size_bytes = static_cast<uint64_t>(p.parseNumber());
          }
          else if (k == "first_event_ns")
          {
            seg.first_event_ns = p.parseNumber();
          }
          else if (k == "last_event_ns")
          {
            seg.last_event_ns = p.parseNumber();
          }
          else if (k == "event_count")
          {
            seg.event_count = static_cast<uint32_t>(p.parseNumber());
          }
          else
          {
            p.skipValue();
          }
        }
        _manifest.segments.push_back(std::move(seg));
      }
    }
    else
    {
      p.skipValue();
    }
  }
  if (_manifest.format_version != kRunFormatVersion)
  {
    throw std::runtime_error("trace reader: unsupported format version");
  }
}

std::optional<TraceManifest::Segment> TraceReader::findSegment(RecordKind kind) const
{
  for (const auto& seg : _manifest.segments)
  {
    if (seg.record_kind == kind)
    {
      return seg;
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> TraceReader::readSegmentBytes(const std::string& name) const
{
  // Segment names come out of the bundle's own manifest, which arrives with
  // the bundle. Appending one unchecked let a manifest name an absolute path
  // or climb out with "..", and the reader would happily open whatever was
  // there: std::filesystem::path("/tmp/run") / "/etc/hosts" is "/etc/hosts".
  const std::filesystem::path rel(name);
  if (rel.is_absolute() || rel.has_root_name())
  {
    throw std::runtime_error("trace reader: segment name must be relative: " + name);
  }
  for (const auto& part : rel)
  {
    if (part == "..")
    {
      throw std::runtime_error("trace reader: segment name escapes the bundle: " + name);
    }
  }

  std::filesystem::path full = std::filesystem::path(_root) / rel;
  std::ifstream in(full, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("trace reader: cannot open segment " + full.string());
  }
  in.seekg(0, std::ios::end);
  size_t sz = static_cast<size_t>(in.tellg());
  in.seekg(0);
  std::vector<uint8_t> buf(sz);
  if (sz > 0)
  {
    in.read(reinterpret_cast<char*>(buf.data()), sz);
  }
  return buf;
}

std::vector<OwnedSignal> TraceReader::readAllSignals()
{
  std::vector<OwnedSignal> out;
  auto seg = findSegment(RecordKind::Signal);
  if (!seg)
  {
    return out;
  }
  auto bytes = readSegmentBytes(seg->name);
  if (bytes.size() < sizeof(RunSegmentHeader))
  {
    throw std::runtime_error("trace reader: signal segment shorter than header");
  }
  RunSegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (!hdr.isValid() || hdr.record_kind != static_cast<uint8_t>(RecordKind::Signal))
  {
    throw std::runtime_error("trace reader: signal segment header invalid");
  }
  size_t cursor = sizeof(hdr);
  out.reserve(safeReserveCount(hdr.event_count, bytes.size(), sizeof(SignalRecord)));
  for (uint32_t i = 0; i < hdr.event_count; ++i)
  {
    if (cursor + sizeof(lrvx::replay::FrameHeader) > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated frame header");
    }
    lrvx::replay::FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + cursor, sizeof(fh));
    cursor += sizeof(fh);
    if (fh.type != static_cast<uint8_t>(FrameType::Signal))
    {
      throw std::runtime_error("trace reader: unexpected frame type in signal segment");
    }
    if (cursor + fh.size > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated signal payload");
    }
    uint32_t crc = lrvx::replay::Crc32::compute(bytes.data() + cursor, fh.size);
    if (crc != fh.crc32)
    {
      throw std::runtime_error("trace reader: signal frame crc mismatch");
    }
    if (fh.size < sizeof(SignalRecord))
    {
      throw std::runtime_error("trace reader: signal frame smaller than record");
    }
    SignalRecord rec{};
    std::memcpy(&rec, bytes.data() + cursor, sizeof(rec));

    // The three lengths live inside the record, which lives inside the bytes
    // the CRC is taken over, so a doctored file checksums clean. Without this
    // cross-check the reader took the lengths at their word and read past the
    // segment: 60,000 bytes for a name out of a 124-byte file, 262,140 for a
    // symbol list, and payload_len is 32-bit, so up to 4 GB. No exception, no
    // crash in the small cases -- just whatever was next in the heap, copied
    // into the signal and carried on into the report and the bundle.
    const size_t declared = static_cast<size_t>(rec.name_len) +
                            static_cast<size_t>(rec.symbol_count) * sizeof(uint32_t) +
                            static_cast<size_t>(rec.payload_len);
    if (declared > fh.size - sizeof(rec))
    {
      throw std::runtime_error(
          "trace reader: signal record lengths overrun the frame");
    }

    OwnedSignal os;
    os.run_ts_ns = rec.run_ts_ns;
    os.feed_ts_ns = rec.feed_ts_ns;
    os.signal_id = rec.signal_id;
    os.signal_id_hi = rec.signal_id_hi;
    os.flags = rec.flags;
    os.strength_raw = rec.strength_raw;
    size_t off = cursor + sizeof(rec);
    if (rec.name_len > 0)
    {
      os.name.assign(reinterpret_cast<const char*>(bytes.data() + off), rec.name_len);
      off += rec.name_len;
    }
    os.symbol_ids.resize(rec.symbol_count);
    for (uint16_t s = 0; s < rec.symbol_count; ++s)
    {
      uint32_t sid = 0;
      std::memcpy(&sid, bytes.data() + off, sizeof(sid));
      os.symbol_ids[s] = sid;
      off += sizeof(sid);
    }
    if (rec.payload_len > 0)
    {
      os.payload.assign(bytes.data() + off, bytes.data() + off + rec.payload_len);
    }
    out.push_back(std::move(os));
    cursor += fh.size;
  }
  return out;
}

std::vector<OwnedOrderEvent> TraceReader::readAllOrderEvents()
{
  std::vector<OwnedOrderEvent> out;
  auto seg = findSegment(RecordKind::OrderEvent);
  if (!seg)
  {
    return out;
  }
  auto bytes = readSegmentBytes(seg->name);
  if (bytes.size() < sizeof(RunSegmentHeader))
  {
    throw std::runtime_error("trace reader: order segment shorter than header");
  }
  RunSegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (!hdr.isValid() || hdr.record_kind != static_cast<uint8_t>(RecordKind::OrderEvent))
  {
    throw std::runtime_error("trace reader: order segment header invalid");
  }
  size_t cursor = sizeof(hdr);
  out.reserve(safeReserveCount(hdr.event_count, bytes.size(), sizeof(OrderEventRecord)));
  for (uint32_t i = 0; i < hdr.event_count; ++i)
  {
    if (cursor + sizeof(lrvx::replay::FrameHeader) > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated order frame header");
    }
    lrvx::replay::FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + cursor, sizeof(fh));
    cursor += sizeof(fh);
    if (fh.type != static_cast<uint8_t>(FrameType::OrderEvent))
    {
      throw std::runtime_error("trace reader: unexpected frame type in order segment");
    }
    if (cursor + fh.size > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated order payload");
    }
    uint32_t crc = lrvx::replay::Crc32::compute(bytes.data() + cursor, fh.size);
    if (crc != fh.crc32)
    {
      throw std::runtime_error("trace reader: order frame crc mismatch");
    }
    if (fh.size < sizeof(OrderEventRecord))
    {
      throw std::runtime_error("trace reader: order frame smaller than record");
    }
    OrderEventRecord rec{};
    std::memcpy(&rec, bytes.data() + cursor, sizeof(rec));

    // Same shape of trust as the signal path above: reason_len is 32-bit and
    // was taken straight from the record, so a doctored file with a valid CRC
    // read up to 4 GB past the segment into the reason string.
    if (static_cast<size_t>(rec.reason_len) > fh.size - sizeof(rec))
    {
      throw std::runtime_error(
          "trace reader: order record reason length overruns the frame");
    }

    OwnedOrderEvent oe;
    oe.run_ts_ns = rec.run_ts_ns;
    oe.feed_ts_ns = rec.feed_ts_ns;
    oe.order_id = rec.order_id;
    oe.parent_signal_id = rec.parent_signal_id;
    oe.price_raw = rec.price_raw;
    oe.qty_raw = rec.qty_raw;
    oe.symbol_id = rec.symbol_id;
    oe.event_kind = static_cast<OrderEventKind>(rec.event_kind);
    oe.side = rec.side;
    oe.order_type = rec.order_type;
    oe.flags = rec.flags;
    if (rec.reason_len > 0)
    {
      oe.reason.assign(reinterpret_cast<const char*>(bytes.data() + cursor + sizeof(rec)),
                       rec.reason_len);
    }
    out.push_back(std::move(oe));
    cursor += fh.size;
  }
  return out;
}

std::vector<FillRecord> TraceReader::readAllFills()
{
  std::vector<FillRecord> out;
  auto seg = findSegment(RecordKind::Fill);
  if (!seg)
  {
    return out;
  }
  auto bytes = readSegmentBytes(seg->name);
  if (bytes.size() < sizeof(RunSegmentHeader))
  {
    throw std::runtime_error("trace reader: fill segment shorter than header");
  }
  RunSegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (!hdr.isValid() || hdr.record_kind != static_cast<uint8_t>(RecordKind::Fill))
  {
    throw std::runtime_error("trace reader: fill segment header invalid");
  }
  size_t cursor = sizeof(hdr);
  out.reserve(safeReserveCount(hdr.event_count, bytes.size(), sizeof(FillRecord)));
  for (uint32_t i = 0; i < hdr.event_count; ++i)
  {
    if (cursor + sizeof(lrvx::replay::FrameHeader) > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated fill frame header");
    }
    lrvx::replay::FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + cursor, sizeof(fh));
    cursor += sizeof(fh);
    if (fh.type != static_cast<uint8_t>(FrameType::Fill))
    {
      throw std::runtime_error("trace reader: unexpected frame type in fill segment");
    }
    if (cursor + fh.size > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated fill payload");
    }
    uint32_t crc = lrvx::replay::Crc32::compute(bytes.data() + cursor, fh.size);
    if (crc != fh.crc32)
    {
      throw std::runtime_error("trace reader: fill frame crc mismatch");
    }
    if (fh.size < sizeof(FillRecord))
    {
      throw std::runtime_error("trace reader: fill frame too small");
    }
    FillRecord rec{};
    std::memcpy(&rec, bytes.data() + cursor, sizeof(rec));
    out.push_back(rec);
    cursor += fh.size;
  }
  return out;
}

}  // namespace lrvx::run
