/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/replay_connector.h"

#include "lrvx/util/base/time.h"
#include "lrvx/util/concurrency/thread_body.h"

#include <chrono>
#include <thread>

namespace lrvx
{

ReplayConnector::ReplayConnector(ReplayConnectorConfig config)
    : _config(std::move(config)), _speed_multiplier(_config.speed.multiplier)
{
  _pmr_resource = std::make_unique<std::pmr::monotonic_buffer_resource>(1024 * 1024);
}

ReplayConnector::~ReplayConnector() { stop(); }

void ReplayConnector::start()
{
  if (_running.exchange(true))
  {
    return;
  }

  replay::ReaderConfig reader_config{
      .data_dir = _config.data_dir,
      .from_ns = _config.from_ns,
      .to_ns = _config.to_ns,
      .symbols = _config.symbols,
  };

  _reader = std::make_unique<replay::BinaryLogReader>(std::move(reader_config));
  _finished.store(false);

  _replay_thread = makeThread("lrvx.replay.connector", [this]
                              { replayLoop(); });
}

void ReplayConnector::stop()
{
  _running.store(false);
  if (_replay_thread.joinable())
  {
    _replay_thread.join();
  }
  _reader.reset();
}

void ReplayConnector::replayLoop()
{
  if (!_reader)
  {
    return;
  }

  int64_t start_from_ts = 0;

  {
    int64_t seek = _seek_target.exchange(-1, std::memory_order_acquire);
    if (seek >= 0)
    {
      start_from_ts = seek;
    }
  }

  auto processEvents = [&](int64_t from_ts)
  {
    int64_t last_event_ts = 0;
    auto wall_start = std::chrono::steady_clock::now();
    int64_t sim_start_ts = 0;

    auto callback = [&](const replay::ReplayEvent& event) -> bool
    {
      if (!_running.load(std::memory_order_relaxed))
      {
        return false;
      }

      int64_t seek = _seek_target.load(std::memory_order_relaxed);
      if (seek >= 0)
      {
        return false;
      }

      _current_pos.store(event.timestamp_ns, std::memory_order_relaxed);

      double speed_mult = _speed_multiplier.load(std::memory_order_relaxed);
      if (speed_mult > 0.0 && last_event_ts > 0)
      {
        int64_t sim_delta_ns = event.timestamp_ns - sim_start_ts;
        auto wall_now = std::chrono::steady_clock::now();
        auto wall_elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_now - wall_start).count();

        int64_t target_wall_ns = static_cast<int64_t>(sim_delta_ns / speed_mult);
        int64_t sleep_ns = target_wall_ns - wall_elapsed_ns;

        if (sleep_ns > 1000000)
        {
          std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        }
      }
      else if (sim_start_ts == 0)
      {
        sim_start_ts = event.timestamp_ns;
        wall_start = std::chrono::steady_clock::now();
      }

      last_event_ts = event.timestamp_ns;

      // Dispatch on the frame's own type. "Trade, else book" turned every
      // side-channel frame into a book update: the reader parses an
      // OptionQuote or a PoolState into its own member and leaves
      // book_header holding the previous book event, so what went out was
      // that book's symbol and timestamp with both sides emptied -- a
      // wipe of a book nobody had touched. The format is additive, so a
      // frame type this build does not publish is skipped, not guessed at.
      switch (event.type)
      {
        case replay::EventType::Trade:
          emitTradeFromRecord(event.trade);
          break;
        case replay::EventType::BookSnapshot:
        case replay::EventType::BookDelta:
        case replay::EventType::BookTop:
          emitBookFromRecord(event.book_header, event.bids, event.asks);
          break;
        case replay::EventType::OptionQuote:
        case replay::EventType::PoolState:
          break;
      }

      return true;
    };

    if (from_ts > 0)
    {
      return _reader->forEachFrom(from_ts, callback);
    }
    else
    {
      return _reader->forEach(callback);
    }
  };

  while (_running.load(std::memory_order_relaxed))
  {
    bool completed = processEvents(start_from_ts);

    int64_t seek = _seek_target.exchange(-1, std::memory_order_acquire);
    if (seek >= 0 && _running.load(std::memory_order_relaxed))
    {
      replay::ReaderConfig reader_config{
          .data_dir = _config.data_dir,
          .from_ns = _config.from_ns,
          .to_ns = _config.to_ns,
          .symbols = _config.symbols,
      };
      _reader = std::make_unique<replay::BinaryLogReader>(std::move(reader_config));
      start_from_ts = seek;
      continue;
    }

    if (completed)
    {
      break;
    }
  }

  _finished.store(true);
}

void ReplayConnector::emitTradeFromRecord(const replay::TradeRecord& record)
{
  TradeEvent event{};
  event.trade.symbol = record.symbol_id;
  event.trade.instrument = static_cast<InstrumentType>(record.instrument);
  event.trade.price = Price::fromRaw(record.price_raw);
  event.trade.quantity = Quantity::fromRaw(record.qty_raw);
  event.trade.isBuy = (record.side == 0);
  event.trade.exchangeTsNs = UnixNanos::fromRaw(record.exchange_ts_ns);

  event.trade_id = record.trade_id;
  // A tape's recv_ts is a wall-clock recording; the live field is monotonic.
  // Replay reuses the field as "position on the recorded timeline", which is
  // the one place the domains legitimately meet -- and now it is explicit.
  event.recvNs = MonoNanos::fromRaw(static_cast<uint64_t>(record.recv_ts_ns));
  event.exchangeMsgTsNs = UnixNanos::fromRaw(record.exchange_ts_ns);
  event.sourceExchange = record.exchange_id;

  emitTrade(event);
}

void ReplayConnector::emitBookFromRecord(const replay::BookRecordHeader& header,
                                         const std::vector<replay::BookLevel>& bids,
                                         const std::vector<replay::BookLevel>& asks)
{
  _pmr_resource->release();

  BookUpdateEvent event(_pmr_resource.get());

  event.update.symbol = header.symbol_id;
  event.update.instrument = static_cast<InstrumentType>(header.instrument);
  event.update.type = (header.type == 0)   ? BookUpdateType::SNAPSHOT
                      : (header.type == 2) ? BookUpdateType::TOP_OF_BOOK
                                           : BookUpdateType::DELTA;
  event.update.exchangeTsNs = UnixNanos::fromRaw(header.exchange_ts_ns);

  event.seq = header.seq;
  event.recvNs = static_cast<MonoNanos>(header.recv_ts_ns);
  event.sourceExchange = header.exchange_id;

  event.update.bids.reserve(bids.size());
  for (const auto& level : bids)
  {
    event.update.bids.emplace_back(Price::fromRaw(level.price_raw),
                                   Quantity::fromRaw(level.qty_raw));
  }

  event.update.asks.reserve(asks.size());
  for (const auto& level : asks)
  {
    event.update.asks.emplace_back(Price::fromRaw(level.price_raw),
                                   Quantity::fromRaw(level.qty_raw));
  }

  emitBookUpdate(event);
}

std::optional<TimeRange> ReplayConnector::dataRange() const
{
  if (!_reader)
  {
    return std::nullopt;
  }

  auto range = _reader->timeRange();
  if (!range)
  {
    return std::nullopt;
  }

  return TimeRange{range->first, range->second};
}

void ReplayConnector::setSpeed(ReplaySpeed speed)
{
  _speed_multiplier.store(speed.multiplier, std::memory_order_relaxed);
}

bool ReplayConnector::seekTo(int64_t timestamp_ns)
{
  if (timestamp_ns < 0)
  {
    return false;
  }

  _seek_target.store(timestamp_ns, std::memory_order_release);
  _finished.store(false, std::memory_order_relaxed);

  return true;
}

bool ReplayConnector::isFinished() const { return _finished.load(std::memory_order_relaxed); }

int64_t ReplayConnector::currentPosition() const
{
  return _current_pos.load(std::memory_order_relaxed);
}

}  // namespace lrvx
