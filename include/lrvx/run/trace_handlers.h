/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/execution/abstract_execution_listener.h"
#include "lrvx/run/trace_recorder.h"
#include "lrvx/strategy/abstract_signal_handler.h"
#include "lrvx/strategy/signal.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace lrvx::run
{

/**
 * The trigger of a conditional order, as the payload of its captured signal.
 * An `OrderEventRecord` has one price field, which holds the order's limit
 * price (0 for a stop or take-profit that fills at market), and no field for
 * a trigger, so capture and reproducibility bundles both write it here, as
 * compact JSON with sorted keys:
 *
 *   stop / take-profit, market or limit   {"trigger_price_raw":N}
 *   trailing stop                         {"trailing_bps":N,"trailing_offset_raw":N}
 *
 * `trigger_price_raw` is the trigger, or the price when the signal names no
 * trigger; raw values are fixed-point at scale 1e8. Every other signal gets
 * an empty payload.
 */
inline std::string conditionalTriggerPayload(const Signal& s)
{
  switch (s.type)
  {
    case SignalType::StopMarket:
    case SignalType::StopLimit:
    case SignalType::TakeProfitMarket:
    case SignalType::TakeProfitLimit:
    {
      const int64_t trigger = s.triggerPrice.raw() > 0 ? s.triggerPrice.raw() : s.price.raw();
      return "{\"trigger_price_raw\":" + std::to_string(trigger) + "}";
    }
    case SignalType::TrailingStop:
      return "{\"trailing_bps\":" + std::to_string(s.trailingCallbackRate) +
             ",\"trailing_offset_raw\":" + std::to_string(s.trailingOffset.raw()) + "}";
    default:
      return {};
  }
}

/**
 * Thin `ISignalHandler` wrapper that forwards to the inner handler and
 * writes every emitted signal into a `TraceRecorder`.
 *
 * Usage: attach this in front of the strategy's normal signal handler
 * during the strategy's wiring so signals get captured automatically.
 *
 * The recorder pointer is held by raw pointer; the caller owns its
 * lifetime and must keep it alive for as long as the handler is in
 * use. Setting it to `nullptr` disables capture without losing the
 * inner handler.
 */
class TraceSignalHandler : public ISignalHandler
{
 public:
  TraceSignalHandler(ISignalHandler* inner, TraceRecorder* recorder)
      : _inner(inner), _recorder(recorder) {}

  void setRecorder(TraceRecorder* rec) noexcept { _recorder = rec; }
  TraceRecorder* recorder() const noexcept { return _recorder; }

  void setFeedTsNs(int64_t ts) noexcept { _feed_ts_ns.store(ts, std::memory_order_relaxed); }

  void onSignal(const Signal& signal) override
  {
    record(signal);
    if (_inner)
    {
      _inner->onSignal(signal);
    }
  }

  // A batch is traced signal by signal but handed to the inner handler
  // whole: re-dispatching through onSignal would turn one coalesced submit
  // into N singles downstream, which is exactly what the batch exists to
  // avoid.
  void onSignalBatch(std::span<const Signal> signals) override
  {
    for (const auto& signal : signals)
    {
      record(signal);
    }
    if (_inner)
    {
      _inner->onSignalBatch(signals);
    }
  }

 private:
  ISignalHandler* _inner;
  TraceRecorder* _recorder;
  std::atomic<int64_t> _feed_ts_ns{0};

  void record(const Signal& signal)
  {
    if (!_recorder)
    {
      return;
    }
    SignalView view;
    view.run_ts_ns = nowNs();
    view.feed_ts_ns = _feed_ts_ns.load(std::memory_order_relaxed);
    view.signal_id = static_cast<uint64_t>(signal.orderId);
    view.flags = encodeFlags(signal);
    view.strength_raw = 0;
    view.symbol_ids = {static_cast<uint32_t>(signal.symbol)};
    view.name = signalTypeName(signal.type);
    const std::string payload = conditionalTriggerPayload(signal);
    view.payload = payload;
    _recorder->writeSignal(view);
  }

  static int64_t nowNs() noexcept
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  static uint32_t encodeFlags(const Signal& s) noexcept
  {
    uint32_t f = 0;
    switch (s.type)
    {
      case SignalType::Market:
      case SignalType::Limit:
      case SignalType::StopMarket:
      case SignalType::StopLimit:
      case SignalType::TakeProfitMarket:
      case SignalType::TakeProfitLimit:
      case SignalType::TrailingStop:
        f |= SignalFlags::Enter;
        break;
      case SignalType::Cancel:
      case SignalType::CancelAll:
        f |= SignalFlags::Exit;
        break;
      default:
        break;
    }
    return f;
  }

  static const char* signalTypeName(SignalType t) noexcept
  {
    switch (t)
    {
      case SignalType::Market:
        return "market";
      case SignalType::Limit:
        return "limit";
      case SignalType::Cancel:
        return "cancel";
      case SignalType::CancelAll:
        return "cancel_all";
      case SignalType::Modify:
        return "modify";
      case SignalType::StopMarket:
        return "stop_market";
      case SignalType::StopLimit:
        return "stop_limit";
      case SignalType::TakeProfitMarket:
        return "take_profit_market";
      case SignalType::TakeProfitLimit:
        return "take_profit_limit";
      case SignalType::TrailingStop:
        return "trailing_stop";
      default:
        return "unknown";
    }
  }
};

/**
 * `IOrderExecutionListener` wrapper that captures order lifecycle
 * events (submit / ack / partial fill / fill / cancel / reject /
 * expire) into a `TraceRecorder`. The inner listener is still called
 * if non-null, so attaching the trace listener does not displace
 * existing behavior.
 */
class TraceExecutionListener : public IOrderExecutionListener
{
 public:
  TraceExecutionListener(SubscriberId id, IOrderExecutionListener* inner,
                         TraceRecorder* recorder)
      : IOrderExecutionListener(id), _inner(inner), _recorder(recorder) {}

  void setRecorder(TraceRecorder* rec) noexcept { _recorder = rec; }
  void setFeedTsNs(int64_t ts) noexcept { _feed_ts_ns.store(ts, std::memory_order_relaxed); }

  void onOrderSubmitted(const Order& o) override
  {
    writeOrderEvent(o, OrderEventKind::Submit);
    if (_inner)
    {
      _inner->onOrderSubmitted(o);
    }
  }
  void onOrderAccepted(const Order& o) override
  {
    writeOrderEvent(o, OrderEventKind::Ack);
    if (_inner)
    {
      _inner->onOrderAccepted(o);
    }
  }
  void onOrderPartiallyFilled(const Order& o, Quantity q) override
  {
    // No fill price in this form either.
    writeFill(o, q, o.price);
    if (_inner)
    {
      _inner->onOrderPartiallyFilled(o, q);
    }
  }
  void onOrderFilled(const Order& o) override
  {
    // This form names no fill price: the order's own is all there is.
    writeFill(o, o.quantity, o.price);
    if (_inner)
    {
      _inner->onOrderFilled(o);
    }
  }
  void onOrderPartiallyFilled(const Order& o, Quantity q, Price fillPrice) override
  {
    writeFill(o, q, fillPrice);
    if (_inner)
    {
      _inner->onOrderPartiallyFilled(o, q, fillPrice);
    }
  }
  void onOrderFilled(const Order& o, Quantity q, Price fillPrice) override
  {
    writeFill(o, q, fillPrice);
    if (_inner)
    {
      _inner->onOrderFilled(o, q, fillPrice);
    }
  }
  void onOrderCanceled(const Order& o) override
  {
    writeOrderEvent(o, OrderEventKind::Cancel);
    if (_inner)
    {
      _inner->onOrderCanceled(o);
    }
  }
  void onOrderRejected(const Order& o, const std::string& reason) override
  {
    writeOrderEvent(o, OrderEventKind::Reject, reason);
    if (_inner)
    {
      _inner->onOrderRejected(o, reason);
    }
  }
  void onOrderExpired(const Order& o) override
  {
    writeOrderEvent(o, OrderEventKind::Expire);
    if (_inner)
    {
      _inner->onOrderExpired(o);
    }
  }

  // The raw fan-out is a real dispatch path, so a wrapper that swallowed it
  // would displace inner behaviour -- exactly what this class promises not to
  // do. Nothing is recorded here: every kind the recorder knows is already
  // written by the typed callbacks above, and writing again would double each
  // traced event.
  void onOrderEvent(const OrderEvent& ev) override
  {
    if (_inner)
    {
      _inner->onOrderEvent(ev);
    }
  }

 private:
  IOrderExecutionListener* _inner;
  TraceRecorder* _recorder;
  std::atomic<int64_t> _feed_ts_ns{0};
  // Fills written so far: a fill's id is its number in the run, 1, 2, 3, ...
  std::atomic<uint64_t> _fills{0};

  static int64_t nowNs() noexcept
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  void writeOrderEvent(const Order& o, OrderEventKind kind,
                       const std::string& reason = "")
  {
    if (!_recorder)
    {
      return;
    }
    OrderEventView e;
    e.run_ts_ns = nowNs();
    e.feed_ts_ns = _feed_ts_ns.load(std::memory_order_relaxed);
    e.order_id = static_cast<uint64_t>(o.id);
    // A captured signal's id is the id of the order it asks for, so the
    // order's events join to it (and to a later cancel or modify of the same
    // order) on this field.
    e.parent_signal_id = static_cast<uint64_t>(o.id);
    e.price_raw = o.price.raw();
    e.qty_raw = o.quantity.raw();
    e.symbol_id = o.symbol;
    e.event_kind = kind;
    e.side = (o.side == Side::BUY) ? 0 : 1;
    e.order_type = static_cast<uint8_t>(o.type);
    e.flags = 0;
    if (!reason.empty())
    {
      e.reason = reason;
    }
    _recorder->writeOrderEvent(e);
  }

  void writeFill(const Order& o, Quantity q, Price fillPrice)
  {
    if (!_recorder)
    {
      return;
    }
    FillView f;
    f.run_ts_ns = nowNs();
    f.feed_ts_ns = _feed_ts_ns.load(std::memory_order_relaxed);
    f.order_id = static_cast<uint64_t>(o.id);
    // Its own id, not the order's: two partial fills of one order used to
    // carry the same fill_id, the order id.
    f.fill_id = _fills.fetch_add(1, std::memory_order_relaxed) + 1;
    f.price_raw = fillPrice.raw();
    f.qty_raw = q.raw();
    f.fee_raw = 0;
    f.symbol_id = o.symbol;
    f.side = (o.side == Side::BUY) ? 0 : 1;
    f.liquidity = FillLiquidity::Unknown;
    _recorder->writeFill(f);
  }
};

}  // namespace lrvx::run
