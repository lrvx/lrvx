/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/run/trace_recorder.h"

#include "lrvx/replay/binary_format_v1.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace lrvx::run
{

namespace
{

int64_t nowNs()
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

const char* recordKindName(RecordKind k)
{
  switch (k)
  {
    case RecordKind::Signal:
      return "signals";
    case RecordKind::OrderEvent:
      return "orders";
    case RecordKind::Fill:
      return "fills";
    default:
      return "unknown";
  }
}

void appendPadding(std::vector<uint8_t>& buf, size_t aligned_size)
{
  if (buf.size() < aligned_size)
  {
    buf.resize(aligned_size, 0);
  }
}

std::string jsonEscape(std::string_view s)
{
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s)
  {
    switch (c)
    {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20)
        {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<uint8_t>(c));
          out += buf;
        }
        else
        {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

TraceRecorder::TraceRecorder(const std::string& path, TraceRecorderOptions options)
    : _root(path), _options(std::move(options))
{
  std::filesystem::create_directories(_root);
  if (_options.run_started_ns == 0)
  {
    _options.run_started_ns = nowNs();
  }
}

void TraceRecorder::addTapeRef(TapeRef ref)
{
  _options.tape_refs.push_back(std::move(ref));
}

TraceRecorder::~TraceRecorder()
{
  try
  {
    close();
  }
  catch (...)
  {
    // Destructor must not throw.
  }
}

TraceRecorder::SegmentWriter& TraceRecorder::openSegment(RecordKind kind)
{
  std::unique_ptr<SegmentWriter>* slot = nullptr;
  switch (kind)
  {
    case RecordKind::Signal:
      slot = &_signals;
      break;
    case RecordKind::OrderEvent:
      slot = &_orders;
      break;
    case RecordKind::Fill:
      slot = &_fills;
      break;
    default:
      throw std::invalid_argument("openSegment: invalid kind");
  }
  if (*slot)
  {
    return **slot;
  }
  auto seg = std::make_unique<SegmentWriter>();
  std::ostringstream oss;
  oss << recordKindName(kind) << "-000000.bin";
  seg->filename = oss.str();
  std::filesystem::path full = std::filesystem::path(_root) / seg->filename;
  seg->stream.open(full, std::ios::binary | std::ios::trunc);
  if (!seg->stream)
  {
    throw std::runtime_error("trace recorder: cannot open " + full.string());
  }
  seg->header.record_kind = static_cast<uint8_t>(kind);
  seg->header.created_ns = nowNs();
  // Reserve space for the header; we patch it in finalizeSegment().
  RunSegmentHeader placeholder{};
  placeholder.record_kind = static_cast<uint8_t>(kind);
  seg->stream.write(reinterpret_cast<const char*>(&placeholder), sizeof(placeholder));
  seg->size_bytes = sizeof(placeholder);
  *slot = std::move(seg);
  return **slot;
}

void TraceRecorder::writeFrameBlob(SegmentWriter& seg, FrameType type,
                                   const void* payload, uint32_t size, int64_t ts)
{
  lrvx::replay::FrameHeader fh;
  fh.size = size;
  fh.crc32 = lrvx::replay::Crc32::compute(payload, size);
  fh.type = static_cast<uint8_t>(type);
  fh.rec_version = 1;
  fh.flags = 0;
  seg.stream.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
  seg.stream.write(reinterpret_cast<const char*>(payload), size);
  seg.size_bytes += sizeof(fh) + size;
  if (seg.event_count == 0 || ts < seg.first_event_ns)
  {
    seg.first_event_ns = ts;
  }
  if (ts > seg.last_event_ns)
  {
    seg.last_event_ns = ts;
  }
  ++seg.event_count;
  if (ts > _run_ended_ns)
  {
    _run_ended_ns = ts;
  }
}

void TraceRecorder::writeSignal(const SignalView& s)
{
  if (_closed)
  {
    throw std::runtime_error("trace recorder: writeSignal after close");
  }
  if (s.name.size() > UINT16_MAX || s.symbol_ids.size() > UINT16_MAX)
  {
    throw std::invalid_argument("trace recorder: signal name/symbol_ids exceed uint16 cap");
  }
  SignalRecord rec{};
  rec.run_ts_ns = s.run_ts_ns;
  rec.feed_ts_ns = s.feed_ts_ns;
  rec.signal_id = static_cast<uint32_t>(s.signal_id);
  rec.signal_id_hi = static_cast<uint32_t>(s.signal_id >> 32);
  rec.name_len = static_cast<uint16_t>(s.name.size());
  rec.symbol_count = static_cast<uint16_t>(s.symbol_ids.size());
  rec.payload_len = static_cast<uint32_t>(s.payload.size());
  rec.flags = s.flags;
  rec.strength_raw = s.strength_raw;
  size_t aligned = signalFrameSize(rec.name_len, rec.symbol_count, rec.payload_len);
  std::vector<uint8_t> buf;
  buf.reserve(aligned);
  buf.resize(sizeof(rec));
  std::memcpy(buf.data(), &rec, sizeof(rec));
  buf.insert(buf.end(), s.name.begin(), s.name.end());
  for (uint32_t sid : s.symbol_ids)
  {
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(&sid),
               reinterpret_cast<const uint8_t*>(&sid) + sizeof(sid));
  }
  buf.insert(buf.end(), s.payload.begin(), s.payload.end());
  appendPadding(buf, aligned);
  auto& seg = openSegment(RecordKind::Signal);
  writeFrameBlob(seg, FrameType::Signal, buf.data(), static_cast<uint32_t>(buf.size()), s.run_ts_ns);
}

void TraceRecorder::writeOrderEvent(const OrderEventView& e)
{
  if (_closed)
  {
    throw std::runtime_error("trace recorder: writeOrderEvent after close");
  }
  if (e.reason.size() > UINT32_MAX)
  {
    throw std::invalid_argument("trace recorder: reason too long");
  }
  OrderEventRecord rec{};
  rec.run_ts_ns = e.run_ts_ns;
  rec.feed_ts_ns = e.feed_ts_ns;
  rec.order_id = e.order_id;
  rec.parent_signal_id = e.parent_signal_id;
  rec.price_raw = e.price_raw;
  rec.qty_raw = e.qty_raw;
  rec.symbol_id = e.symbol_id;
  rec.event_kind = static_cast<uint8_t>(e.event_kind);
  rec.side = e.side;
  rec.order_type = e.order_type;
  rec.reason_len = static_cast<uint32_t>(e.reason.size());
  rec.flags = e.flags;
  size_t aligned = orderEventFrameSize(rec.reason_len);
  std::vector<uint8_t> buf;
  buf.reserve(aligned);
  buf.resize(sizeof(rec));
  std::memcpy(buf.data(), &rec, sizeof(rec));
  buf.insert(buf.end(), e.reason.begin(), e.reason.end());
  appendPadding(buf, aligned);
  auto& seg = openSegment(RecordKind::OrderEvent);
  writeFrameBlob(seg, FrameType::OrderEvent, buf.data(), static_cast<uint32_t>(buf.size()), e.run_ts_ns);
}

void TraceRecorder::writeFill(const FillView& f)
{
  if (_closed)
  {
    throw std::runtime_error("trace recorder: writeFill after close");
  }
  FillRecord rec{};
  rec.run_ts_ns = f.run_ts_ns;
  rec.feed_ts_ns = f.feed_ts_ns;
  rec.order_id = f.order_id;
  rec.fill_id = f.fill_id;
  rec.price_raw = f.price_raw;
  rec.qty_raw = f.qty_raw;
  rec.fee_raw = f.fee_raw;
  rec.symbol_id = f.symbol_id;
  rec.side = f.side;
  rec.liquidity = static_cast<uint8_t>(f.liquidity);
  auto& seg = openSegment(RecordKind::Fill);
  writeFrameBlob(seg, FrameType::Fill, &rec, sizeof(rec), f.run_ts_ns);
}

void TraceRecorder::finalizeSegment(SegmentWriter& seg)
{
  if (!seg.stream.is_open())
  {
    return;
  }
  RunSegmentHeader patched{};
  patched.magic = kRunSegmentMagic;
  patched.version = kRunFormatVersion;
  patched.flags = RunSegmentFlags::Sorted;
  patched.record_kind = seg.header.record_kind;
  patched.created_ns = seg.header.created_ns;
  patched.first_event_ns = seg.first_event_ns;
  patched.last_event_ns = seg.last_event_ns;
  patched.event_count = seg.event_count;
  patched.compression = 0;
  seg.stream.flush();
  seg.stream.seekp(0);
  seg.stream.write(reinterpret_cast<const char*>(&patched), sizeof(patched));
  seg.stream.flush();
  seg.stream.close();
}

void TraceRecorder::writeManifest()
{
  std::filesystem::path manifest_path = std::filesystem::path(_root) / "manifest.json";
  std::ofstream out(manifest_path, std::ios::trunc);
  if (!out)
  {
    throw std::runtime_error("trace recorder: cannot write manifest " + manifest_path.string());
  }
  auto runEnded = _run_ended_ns != 0 ? _run_ended_ns : _options.run_started_ns;
  out << "{\n";
  out << "  \"schema_version\": 1,\n";
  out << "  \"format_version\": " << kRunFormatVersion << ",\n";
  out << "  \"strategy_id\": \"" << jsonEscape(_options.strategy_id) << "\",\n";
  out << "  \"strategy_hash\": \"" << jsonEscape(_options.strategy_hash) << "\",\n";
  out << "  \"run_started_ns\": " << _options.run_started_ns << ",\n";
  out << "  \"run_ended_ns\": " << runEnded << ",\n";
  out << "  \"tape_refs\": [";
  for (size_t i = 0; i < _options.tape_refs.size(); ++i)
  {
    const auto& t = _options.tape_refs[i];
    out << (i == 0 ? "\n" : ",\n");
    out << "    {\"path\": \"" << jsonEscape(t.path) << "\", "
        << "\"content_hash\": \"" << jsonEscape(t.content_hash) << "\", "
        << "\"first_event_ns\": " << t.first_event_ns << ", "
        << "\"last_event_ns\": " << t.last_event_ns << "}";
  }
  if (!_options.tape_refs.empty())
  {
    out << "\n  ";
  }
  out << "],\n";
  out << "  \"segments\": [";
  bool first = true;
  auto emitSegment = [&](const SegmentWriter* seg, const char* kind)
  {
    if (!seg)
    {
      return;
    }
    out << (first ? "\n" : ",\n");
    first = false;
    out << "    {\"name\": \"" << jsonEscape(seg->filename) << "\", "
        << "\"record_kind\": \"" << kind << "\", "
        << "\"size_bytes\": " << seg->size_bytes << ", "
        << "\"first_event_ns\": " << seg->first_event_ns << ", "
        << "\"last_event_ns\": " << seg->last_event_ns << ", "
        << "\"event_count\": " << seg->event_count << "}";
  };
  emitSegment(_signals.get(), "signals");
  emitSegment(_orders.get(), "orders");
  emitSegment(_fills.get(), "fills");
  if (!first)
  {
    out << "\n  ";
  }
  out << "]\n}\n";
}

void TraceRecorder::close()
{
  if (_closed)
  {
    return;
  }
  _closed = true;
  if (_signals)
  {
    finalizeSegment(*_signals);
  }
  if (_orders)
  {
    finalizeSegment(*_orders);
  }
  if (_fills)
  {
    finalizeSegment(*_fills);
  }
  writeManifest();
}

}  // namespace lrvx::run
