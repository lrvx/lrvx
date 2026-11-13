/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/run/trace_reader.h"
#include "lrvx/run/trace_recorder.h"

#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/run/run_format_v1.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{

std::filesystem::path makeTmp(const std::string& tag)
{
  auto p = std::filesystem::temp_directory_path() / ("lrvx_run_test_" + tag);
  std::filesystem::remove_all(p);
  return p;
}

}  // namespace

TEST(RunTrace, RoundTripsSignalsOrdersFills)
{
  using namespace lrvx::run;
  auto tmp = makeTmp("roundtrip");

  TraceRecorderOptions opts;
  opts.strategy_id = "smoke";
  opts.strategy_hash = "sha256:test";
  opts.run_started_ns = 1'700'000'000'000'000'000LL;
  TapeRef tref;
  tref.path = "BTCUSDT.lrvx";
  tref.content_hash = "sha256:abc";
  tref.first_event_ns = 1'700'000'000'000'000'000LL;
  tref.last_event_ns = 1'700'000'000'500'000'000LL;
  opts.tape_refs.push_back(tref);

  {
    TraceRecorder rec(tmp.string(), opts);

    SignalView s;
    s.run_ts_ns = 1'700'000'000'100'000'000LL;
    s.feed_ts_ns = 1'700'000'000'099'000'000LL;
    s.signal_id = 42;
    s.flags = SignalFlags::Enter;
    s.strength_raw = 75'000'000;
    s.name = "ratio-cross";
    s.symbol_ids = {1u, 2u};
    std::string payload = "{\"src\":\"ETH\",\"dst\":\"BTC\"}";
    s.payload = payload;
    rec.writeSignal(s);

    OrderEventView e;
    e.run_ts_ns = 1'700'000'000'200'000'000LL;
    e.feed_ts_ns = 1'700'000'000'099'000'000LL;
    e.order_id = 7;
    e.parent_signal_id = 42;
    e.price_raw = 50'000'00000000LL;
    e.qty_raw = 100'000'000LL;
    e.symbol_id = 1;
    e.event_kind = OrderEventKind::Submit;
    e.side = 0;
    e.order_type = 1;
    e.flags = OrderEventFlags::PostOnly;
    rec.writeOrderEvent(e);

    FillView f;
    f.run_ts_ns = 1'700'000'000'300'000'000LL;
    f.feed_ts_ns = 1'700'000'000'250'000'000LL;
    f.order_id = 7;
    f.fill_id = 12345;
    f.price_raw = 50'000'00000000LL;
    f.qty_raw = 100'000'000LL;
    f.fee_raw = 50'000LL;
    f.symbol_id = 1;
    f.side = 0;
    f.liquidity = FillLiquidity::Maker;
    rec.writeFill(f);

    rec.setRunEndedNs(1'700'000'000'400'000'000LL);
    rec.close();
  }

  TraceReader reader(tmp.string());
  const auto& m = reader.manifest();
  EXPECT_EQ(m.format_version, 1u);
  EXPECT_EQ(m.strategy_id, "smoke");
  ASSERT_EQ(m.tape_refs.size(), 1u);
  EXPECT_EQ(m.tape_refs[0].path, "BTCUSDT.lrvx");
  EXPECT_EQ(m.run_ended_ns, 1'700'000'000'400'000'000LL);

  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].signal_id, 42u);
  EXPECT_EQ(sigs[0].name, "ratio-cross");
  ASSERT_EQ(sigs[0].symbol_ids.size(), 2u);
  EXPECT_EQ(sigs[0].symbol_ids[0], 1u);
  EXPECT_EQ(sigs[0].symbol_ids[1], 2u);
  EXPECT_EQ(std::string(sigs[0].payload.begin(), sigs[0].payload.end()),
            "{\"src\":\"ETH\",\"dst\":\"BTC\"}");
  EXPECT_EQ(sigs[0].flags, lrvx::run::SignalFlags::Enter);

  auto orders = reader.readAllOrderEvents();
  ASSERT_EQ(orders.size(), 1u);
  EXPECT_EQ(orders[0].order_id, 7u);
  EXPECT_EQ(orders[0].parent_signal_id, 42u);
  EXPECT_EQ(orders[0].event_kind, OrderEventKind::Submit);
  EXPECT_EQ(orders[0].flags, lrvx::run::OrderEventFlags::PostOnly);

  auto fills = reader.readAllFills();
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].order_id, 7u);
  EXPECT_EQ(fills[0].fill_id, 12345u);
  EXPECT_EQ(fills[0].liquidity, static_cast<uint8_t>(FillLiquidity::Maker));

  std::filesystem::remove_all(tmp);
}

TEST(RunTrace, EmptySegmentsAreOmitted)
{
  using namespace lrvx::run;
  auto tmp = makeTmp("empty");
  TraceRecorderOptions opts;
  opts.strategy_id = "empty";
  opts.run_started_ns = 1'700'000'000'000'000'000LL;
  {
    TraceRecorder rec(tmp.string(), opts);
    rec.close();
  }
  TraceReader reader(tmp.string());
  EXPECT_TRUE(reader.manifest().segments.empty());
  EXPECT_TRUE(reader.readAllSignals().empty());
  EXPECT_TRUE(reader.readAllOrderEvents().empty());
  EXPECT_TRUE(reader.readAllFills().empty());
  std::filesystem::remove_all(tmp);
}

TEST(RunTrace, MultipleSignalsPreserveOrder)
{
  using namespace lrvx::run;
  auto tmp = makeTmp("order");
  TraceRecorderOptions opts;
  opts.strategy_id = "ord";
  opts.run_started_ns = 1'000'000'000LL;
  {
    TraceRecorder rec(tmp.string(), opts);
    for (uint32_t i = 0; i < 32; ++i)
    {
      SignalView s;
      s.run_ts_ns = 1'000'000'000LL + i * 1000;
      s.signal_id = i;
      s.name = "tick";
      s.symbol_ids = {i};
      rec.writeSignal(s);
    }
    rec.close();
  }
  TraceReader reader(tmp.string());
  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 32u);
  for (uint32_t i = 0; i < 32; ++i)
  {
    EXPECT_EQ(sigs[i].signal_id, i);
    ASSERT_EQ(sigs[i].symbol_ids.size(), 1u);
    EXPECT_EQ(sigs[i].symbol_ids[0], i);
  }
  std::filesystem::remove_all(tmp);
}

// ============================================================================
// Hostile bundles
// ============================================================================
//
// A .lrvxrun bundle travels: it is the artifact you hand someone to reproduce
// a run. The reader used to take the record's own length fields at their
// word, and those fields sit inside the bytes the CRC is taken over, so a
// doctored file checksums clean. A 124-byte segment could ask for 60,000
// bytes of name, 262,140 bytes of symbol ids or -- reason_len and payload_len
// being 32-bit -- up to 4 GB of payload, and the reader would copy whatever
// was next in the heap into the signal and carry it on into the report.

namespace
{

struct HostileBundle
{
  std::filesystem::path root;
  std::filesystem::path segment;
};

// A one-signal bundle with correct magic, frame size and CRC. `mutate` gets
// the record before the CRC is computed, so whatever it writes is covered by
// a checksum that matches.
template <typename Mutate>
HostileBundle writeSignalBundle(const std::string& tag, Mutate&& mutate,
                                const std::string& segmentName = "signals.seg")
{
  using namespace lrvx::run;

  auto root = makeTmp(tag);
  std::filesystem::create_directories(root);

  RunSegmentHeader hdr{};
  hdr.record_kind = static_cast<uint8_t>(RecordKind::Signal);
  hdr.event_count = 1;

  SignalRecord rec{};
  rec.run_ts_ns = 1;
  rec.feed_ts_ns = 1;
  rec.signal_id = 1;
  mutate(rec);

  lrvx::replay::FrameHeader fh{};
  fh.size = sizeof(rec);
  fh.type = static_cast<uint8_t>(FrameType::Signal);
  fh.crc32 = lrvx::replay::Crc32::compute(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));

  auto segPath = root / segmentName;
  {
    std::ofstream out(segPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
  }

  {
    std::ofstream out(root / "manifest.json", std::ios::trunc);
    out << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"format_version\": 1,\n"
        << "  \"strategy_id\": \"hostile\",\n"
        << "  \"strategy_hash\": \"sha256:0\",\n"
        << "  \"run_started_ns\": 1,\n"
        << "  \"run_ended_ns\": 2,\n"
        << "  \"tape_refs\": [],\n"
        << "  \"segments\": [\n"
        << "    {\"name\": \"" << segmentName << "\", \"record_kind\": \"signals\", "
        << "\"size_bytes\": 124, \"first_event_ns\": 1, \"last_event_ns\": 1, "
        << "\"event_count\": 1}\n"
        << "  ]\n}\n";
  }

  return {root, segPath};
}

}  // namespace

TEST(RunTraceHostile, OversizedNameLengthIsRejected)
{
  auto bundle = writeSignalBundle("name_len", [](lrvx::run::SignalRecord& r)
                                  { r.name_len = 60000; });

  lrvx::run::TraceReader reader(bundle.root.string());
  EXPECT_THROW(reader.readAllSignals(), std::runtime_error);

  std::filesystem::remove_all(bundle.root);
}

TEST(RunTraceHostile, OversizedSymbolCountIsRejected)
{
  auto bundle = writeSignalBundle("symbol_count", [](lrvx::run::SignalRecord& r)
                                  { r.symbol_count = 65535; });

  lrvx::run::TraceReader reader(bundle.root.string());
  EXPECT_THROW(reader.readAllSignals(), std::runtime_error);

  std::filesystem::remove_all(bundle.root);
}

TEST(RunTraceHostile, OversizedPayloadLengthIsRejected)
{
  auto bundle = writeSignalBundle("payload_len", [](lrvx::run::SignalRecord& r)
                                  { r.payload_len = 2'000'000'000u; });

  lrvx::run::TraceReader reader(bundle.root.string());
  EXPECT_THROW(reader.readAllSignals(), std::runtime_error);

  std::filesystem::remove_all(bundle.root);
}

// A record whose declared lengths fit exactly is still read. The frame here
// carries no trailing bytes, so every length is zero and the signal comes
// back empty rather than throwing.
TEST(RunTraceHostile, ZeroLengthsStillRead)
{
  auto bundle = writeSignalBundle("zero_len", [](lrvx::run::SignalRecord&) {});

  lrvx::run::TraceReader reader(bundle.root.string());
  auto signals = reader.readAllSignals();
  ASSERT_EQ(signals.size(), 1u);
  EXPECT_TRUE(signals[0].name.empty());
  EXPECT_TRUE(signals[0].symbol_ids.empty());
  EXPECT_TRUE(signals[0].payload.empty());

  std::filesystem::remove_all(bundle.root);
}

// A segment name out of the manifest is appended to the bundle root. Taken
// unchecked, an absolute path replaced the root outright and "../" climbed
// out of it.
TEST(RunTraceHostile, SegmentNameCannotEscapeTheBundle)
{
  auto bundle = writeSignalBundle("escape_abs", [](lrvx::run::SignalRecord&) {}, "/etc/hosts");
  {
    lrvx::run::TraceReader reader(bundle.root.string());
    EXPECT_THROW(reader.readAllSignals(), std::runtime_error);
  }
  std::filesystem::remove_all(bundle.root);

  auto climb = writeSignalBundle("escape_rel", [](lrvx::run::SignalRecord&) {}, "../../etc/hosts");
  {
    lrvx::run::TraceReader reader(climb.root.string());
    EXPECT_THROW(reader.readAllSignals(), std::runtime_error);
  }
  std::filesystem::remove_all(climb.root);
}

// The 64-bit accessors over a record whose id does not fit 32 bits, and over
// one that does.
TEST(RunTrace, SignalIdsReadBackWhole)
{
  auto tmp = makeTmp("signal_id64");
  const uint64_t wide = (uint64_t{0x1F'FFFE} << 32) + 9;
  {
    lrvx::run::TraceRecorderOptions opts;
    opts.strategy_id = "id64";
    lrvx::run::TraceRecorder rec(tmp.string(), std::move(opts));
    lrvx::run::SignalView s;
    s.run_ts_ns = 1;
    s.name = "limit";
    s.symbol_ids = {1};
    s.signal_id = wide;
    rec.writeSignal(s);
    s.run_ts_ns = 2;
    s.signal_id = 42;
    rec.writeSignal(s);
    rec.close();
  }

  {
    lrvx::run::TraceReader reader(tmp.string());
    const auto sigs = reader.readAllSignals();
    ASSERT_EQ(sigs.size(), 2u);
    EXPECT_EQ(sigs[0].signal_id, 9u);
    EXPECT_EQ(sigs[0].signalId64(), wide);
    EXPECT_EQ(sigs[1].signal_id, 42u);
    EXPECT_EQ(sigs[1].signalId64(), 42u);
  }

  std::filesystem::remove_all(tmp);
}

// A record written before the high half existed has zeros there and reads
// back as the id it always was.
TEST(RunTrace, ARecordFromBeforeTheHighHalfReadsItsLowHalf)
{
  lrvx::run::SignalRecord rec{};
  rec.signal_id = 77;
  EXPECT_EQ(rec.signalId64(), 77u);
  EXPECT_EQ(sizeof(lrvx::run::SignalRecord), 48u);
  EXPECT_EQ(offsetof(lrvx::run::SignalRecord, signal_id), 16u);
  EXPECT_EQ(offsetof(lrvx::run::SignalRecord, signal_id_hi), 40u);
}
