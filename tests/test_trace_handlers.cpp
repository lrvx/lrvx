/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/run/trace_handlers.h"
#include "lrvx/run/trace_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

class CountingSignalSink : public lrvx::ISignalHandler
{
 public:
  int count = 0;
  void onSignal(const lrvx::Signal&) override { ++count; }
};

class CountingExecutionSink : public lrvx::IOrderExecutionListener
{
 public:
  using lrvx::IOrderExecutionListener::IOrderExecutionListener;
  int submits = 0;
  int fills = 0;
  void onOrderSubmitted(const lrvx::Order&) override { ++submits; }
  void onOrderFilled(const lrvx::Order&) override { ++fills; }
};

lrvx::run::TraceRecorder makeRec(const std::filesystem::path& p)
{
  std::filesystem::remove_all(p);
  lrvx::run::TraceRecorderOptions opts;
  opts.strategy_id = "trace-handlers-test";
  opts.run_started_ns = 1'700'000'000'000'000'000LL;
  return lrvx::run::TraceRecorder(p.string(), std::move(opts));
}

}  // namespace

TEST(TraceHandlers, SignalHandlerForwardsAndRecords)
{
  auto tmp = std::filesystem::temp_directory_path() / "lrvx_trace_handler_sig";
  auto rec = makeRec(tmp);
  CountingSignalSink inner;
  lrvx::run::TraceSignalHandler h(&inner, &rec);
  h.setFeedTsNs(1'700'000'000'100'000'000LL);

  auto buy = lrvx::Signal::marketBuy(1, lrvx::Quantity::fromDouble(0.5), 42);
  h.onSignal(buy);
  auto sell = lrvx::Signal::marketSell(2, lrvx::Quantity::fromDouble(0.7), 43);
  h.onSignal(sell);

  rec.close();
  EXPECT_EQ(inner.count, 2);

  lrvx::run::TraceReader reader(tmp.string());
  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 2u);
  EXPECT_EQ(sigs[0].signal_id, 42u);
  EXPECT_EQ(sigs[1].signal_id, 43u);
  EXPECT_EQ(sigs[0].feed_ts_ns, 1'700'000'000'100'000'000LL);
  EXPECT_NE(sigs[0].name, "");
  std::filesystem::remove_all(tmp);
}

TEST(TraceHandlers, ExecutionListenerCapturesEvents)
{
  auto tmp = std::filesystem::temp_directory_path() / "lrvx_trace_handler_exec";
  auto rec = makeRec(tmp);
  CountingExecutionSink inner(1);
  lrvx::run::TraceExecutionListener h(2, &inner, &rec);
  h.setFeedTsNs(1'700'000'000'200'000'000LL);

  lrvx::Order o;
  o.id = 7;
  o.symbol = 1;
  o.side = lrvx::Side::BUY;
  o.type = lrvx::OrderType::LIMIT;
  o.price = lrvx::Price::fromDouble(50000.0);
  o.quantity = lrvx::Quantity::fromDouble(0.1);

  h.onOrderSubmitted(o);
  h.onOrderAccepted(o);
  h.onOrderFilled(o);
  rec.close();

  EXPECT_EQ(inner.submits, 1);
  EXPECT_EQ(inner.fills, 1);

  lrvx::run::TraceReader reader(tmp.string());
  auto events = reader.readAllOrderEvents();
  auto fills = reader.readAllFills();
  ASSERT_EQ(events.size(), 2u);  // submit + ack (fill goes to fills segment)
  EXPECT_EQ(events[0].order_id, 7u);
  EXPECT_EQ(events[0].event_kind, lrvx::run::OrderEventKind::Submit);
  EXPECT_EQ(events[1].event_kind, lrvx::run::OrderEventKind::Ack);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].order_id, 7u);
  EXPECT_NEAR(static_cast<double>(fills[0].price_raw) / 1e8, 50000.0, 1e-6);
  std::filesystem::remove_all(tmp);
}

// Every fill was written with fill_id = order id, so the partial fills of one
// order were indistinguishable, and at the order's limit price, not the
// price it filled at.
TEST(TraceHandlers, EachCapturedFillHasItsOwnIdAndItsOwnPrice)
{
  auto tmp = std::filesystem::temp_directory_path() / "lrvx_trace_handler_fill_ids";
  auto rec = makeRec(tmp);
  lrvx::run::TraceExecutionListener h(2, nullptr, &rec);

  lrvx::Order first;
  first.id = (uint64_t{3} << 32) + 7;
  first.symbol = 1;
  first.side = lrvx::Side::BUY;
  first.type = lrvx::OrderType::LIMIT;
  first.price = lrvx::Price::fromDouble(50000.0);
  first.quantity = lrvx::Quantity::fromDouble(1.0);
  lrvx::Order second = first;
  second.id = (uint64_t{3} << 32) + 8;

  h.onOrderPartiallyFilled(first, lrvx::Quantity::fromDouble(0.4),
                           lrvx::Price::fromDouble(49990.0));
  h.onOrderFilled(second, lrvx::Quantity::fromDouble(1.0), lrvx::Price::fromDouble(49995.0));
  h.onOrderFilled(first, lrvx::Quantity::fromDouble(0.6), lrvx::Price::fromDouble(49980.0));
  rec.close();

  lrvx::run::TraceReader reader(tmp.string());
  const auto fills = reader.readAllFills();
  ASSERT_EQ(fills.size(), 3u);
  EXPECT_EQ(fills[0].fill_id, 1u);
  EXPECT_EQ(fills[1].fill_id, 2u);
  EXPECT_EQ(fills[2].fill_id, 3u);
  EXPECT_EQ(fills[0].order_id, first.id);
  EXPECT_EQ(fills[1].order_id, second.id);
  EXPECT_EQ(fills[2].order_id, first.id);
  EXPECT_EQ(fills[0].price_raw, lrvx::Price::fromDouble(49990.0).raw());
  EXPECT_EQ(fills[2].price_raw, lrvx::Price::fromDouble(49980.0).raw());
  EXPECT_EQ(fills[0].qty_raw, lrvx::Quantity::fromDouble(0.4).raw());
  std::filesystem::remove_all(tmp);
}

TEST(TraceHandlers, NullRecorderDisablesCaptureWithoutBreakingForward)
{
  CountingSignalSink inner;
  lrvx::run::TraceSignalHandler h(&inner, nullptr);
  auto buy = lrvx::Signal::marketBuy(1, lrvx::Quantity::fromDouble(1.0), 99);
  h.onSignal(buy);
  EXPECT_EQ(inner.count, 1);
}

namespace
{

// The fixed part of the first signal record in the trace, as it is on disk.
std::vector<uint8_t> firstSignalRecordBytes(const std::filesystem::path& trace)
{
  std::ifstream in(trace / "signals-000000.bin", std::ios::binary);
  std::vector<uint8_t> all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  constexpr size_t kSegmentHeader = 64;
  constexpr size_t kFrameHeader = 12;
  constexpr size_t kRecord = 48;
  if (all.size() < kSegmentHeader + kFrameHeader + kRecord)
  {
    return {};
  }
  const auto start = all.begin() + kSegmentHeader + kFrameHeader;
  return std::vector<uint8_t>(start, start + kRecord);
}

uint32_t u32At(const std::vector<uint8_t>& bytes, size_t offset)
{
  uint32_t v = 0;
  std::memcpy(&v, bytes.data() + offset, sizeof v);
  return v;
}

}  // namespace

// A strategy's order ids are (subscriber id << 32) + n. Capture kept the low
// 32 bits of the id and nothing else, so every strategy's first order was
// signal 1. The low half stays at offset 16, where a reader that predates the
// change looks; the high half is in the first four of the bytes that were
// reserved.
TEST(TraceHandlers, ACapturedSignalKeepsItsWholeOrderId)
{
  auto tmp = std::filesystem::temp_directory_path() / "lrvx_trace_handler_wide_id";
  auto rec = makeRec(tmp);
  lrvx::run::TraceSignalHandler h(nullptr, &rec);
  const lrvx::OrderId wide = (lrvx::OrderId{7} << 32) + 5;
  h.onSignal(lrvx::Signal::marketBuy(1, lrvx::Quantity::fromDouble(0.5), wide));
  rec.close();

  const auto bytes = firstSignalRecordBytes(tmp);
  ASSERT_EQ(bytes.size(), 48u);
  EXPECT_EQ(u32At(bytes, 16), 5u) << "signal_id: the low half, where it always was";
  EXPECT_EQ(u32At(bytes, 40), 7u) << "signal_id_hi: the high half, in the reserved bytes";
  EXPECT_EQ(u32At(bytes, 44), 0u) << "what is left of the reserved bytes";

  lrvx::run::TraceReader reader(tmp.string());
  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 1u);
  EXPECT_EQ(sigs[0].signal_id, 5u) << "the 32-bit field reads as before";
  std::filesystem::remove_all(tmp);
}

// With an id below 2^32 the record is what it was: the bytes that used to be
// reserved are still zero.
TEST(TraceHandlers, ASignalIdBelowTwoToThe32LeavesTheReservedBytesZero)
{
  auto tmp = std::filesystem::temp_directory_path() / "lrvx_trace_handler_narrow_id";
  auto rec = makeRec(tmp);
  lrvx::run::TraceSignalHandler h(nullptr, &rec);
  h.onSignal(lrvx::Signal::marketBuy(1, lrvx::Quantity::fromDouble(0.5), 0xFFFF'FFFFULL));
  rec.close();

  const auto bytes = firstSignalRecordBytes(tmp);
  ASSERT_EQ(bytes.size(), 48u);
  EXPECT_EQ(u32At(bytes, 16), 0xFFFF'FFFFu);
  for (size_t i = 40; i < 48; ++i)
  {
    EXPECT_EQ(bytes[i], 0u) << "byte " << i;
  }
  std::filesystem::remove_all(tmp);
}

// Captured order events carried parent_signal_id 0, so nothing joined them to
// the signal that asked for the order.
TEST(TraceHandlers, CapturedOrderEventsNameTheirSignal)
{
  auto tmp = std::filesystem::temp_directory_path() / "lrvx_trace_handler_parent";
  auto rec = makeRec(tmp);
  lrvx::run::TraceSignalHandler signals(nullptr, &rec);
  lrvx::run::TraceExecutionListener listener(2, nullptr, &rec);

  const lrvx::OrderId first = (lrvx::OrderId{7} << 32) + 1;
  const lrvx::OrderId second = (lrvx::OrderId{7} << 32) + 2;
  signals.onSignal(lrvx::Signal::marketBuy(1, lrvx::Quantity::fromDouble(0.5), first));
  signals.onSignal(lrvx::Signal::marketSell(1, lrvx::Quantity::fromDouble(0.5), second));

  lrvx::Order a;
  a.id = first;
  a.symbol = 1;
  a.quantity = lrvx::Quantity::fromDouble(0.5);
  lrvx::Order b = a;
  b.id = second;
  b.side = lrvx::Side::SELL;
  listener.onOrderSubmitted(a);
  listener.onOrderAccepted(a);
  listener.onOrderSubmitted(b);
  listener.onOrderRejected(b, "no");
  listener.onOrderCanceled(a);
  rec.close();

  lrvx::run::TraceReader reader(tmp.string());
  const auto events = reader.readAllOrderEvents();
  ASSERT_EQ(events.size(), 5u);
  for (const auto& ev : events)
  {
    EXPECT_EQ(ev.parent_signal_id, ev.order_id);
  }
  EXPECT_EQ(events[0].parent_signal_id, first);
  EXPECT_EQ(events[2].parent_signal_id, second);
  EXPECT_EQ(events[3].parent_signal_id, second);
  EXPECT_EQ(events[4].parent_signal_id, first);
  std::filesystem::remove_all(tmp);
}

// The trigger of a conditional order goes into its captured signal's
// payload, with the keys a reproducibility bundle writes: an order event
// has one price field, the limit price, and none for a trigger.
TEST(TraceHandlers, ConditionalSignalsCarryTheirTrigger)
{
  using lrvx::Price;
  using lrvx::Quantity;
  using lrvx::Side;
  using lrvx::Signal;
  auto tmp = std::filesystem::temp_directory_path() / "lrvx_trace_handler_trigger";
  auto rec = makeRec(tmp);
  lrvx::run::TraceSignalHandler h(nullptr, &rec);
  const auto q = Quantity::fromDouble(1.0);
  h.onSignal(Signal::stopMarket(1, Side::SELL, Price::fromDouble(95.0), q, 1));
  h.onSignal(
      Signal::stopLimit(1, Side::SELL, Price::fromDouble(94.0), Price::fromDouble(93.5), q, 2));
  h.onSignal(Signal::trailingStopPercent(1, Side::SELL, 150, q, 3));
  h.onSignal(Signal::marketBuy(1, q, 4));
  rec.close();

  lrvx::run::TraceReader reader(tmp.string());
  auto sigs = reader.readAllSignals();
  ASSERT_EQ(sigs.size(), 4u);
  const auto text = [&](size_t i)
  { return std::string(sigs[i].payload.begin(), sigs[i].payload.end()); };
  EXPECT_EQ(text(0), "{\"trigger_price_raw\":9500000000}");
  EXPECT_EQ(text(1), "{\"trigger_price_raw\":9400000000}");
  EXPECT_EQ(text(2), "{\"trailing_bps\":150,\"trailing_offset_raw\":0}");
  EXPECT_EQ(text(3), "");
  std::filesystem::remove_all(tmp);
}
