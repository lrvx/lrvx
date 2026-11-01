/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/aggregators/bin_count.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace lrvx::replay
{

namespace
{
// Floor ts_ns to the start of its bucket. Handles negative timestamps
// by flooring toward minus-infinity rather than toward zero, so a tape
// with timestamps either side of the unix epoch buckets symmetrically.
int64_t bucketStart(int64_t ts_ns, int64_t bucket_ns)
{
  if (ts_ns >= 0)
  {
    return (ts_ns / bucket_ns) * bucket_ns;
  }
  // For negative ts, integer division truncates toward zero; correct
  // by snapping down by one bucket unless ts is exactly on a boundary.
  int64_t q = ts_ns / bucket_ns;
  if (ts_ns % bucket_ns != 0)
  {
    --q;
  }
  return q * bucket_ns;
}
}  // namespace

BinCountAggregator::BinCountAggregator(int64_t bucket_ns, bool by_side, bool by_symbol,
                                       AggregatorEventFilter event_filter,
                                       std::vector<uint32_t> symbol_filter)
    : _bucket_ns(bucket_ns),
      _by_side(by_side),
      _by_symbol(by_symbol),
      _event_filter(event_filter),
      _symbol_filter(std::move(symbol_filter))
{
  if (bucket_ns <= 0)
  {
    throw std::invalid_argument("BinCountAggregator: bucket_ns must be > 0");
  }
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool BinCountAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void BinCountAggregator::onEvent(const ReplayEvent& ev)
{
  uint32_t symbol_id = 0;
  uint8_t raw_side = 0;
  bool is_trade = false;

  switch (ev.type)
  {
    case EventType::Trade:
      if (_event_filter == AggregatorEventFilter::BooksOnly)
      {
        return;
      }
      symbol_id = ev.trade.symbol_id;
      raw_side = ev.trade.side;
      is_trade = true;
      break;
    case EventType::BookSnapshot:
    case EventType::BookDelta:
    case EventType::BookTop:
      if (_event_filter == AggregatorEventFilter::Trades)
      {
        return;
      }
      symbol_id = ev.book_header.symbol_id;
      break;
    default:
      return;
  }

  if (!symbolAllowed(symbol_id))
  {
    return;
  }

  const int64_t bucket = bucketStart(ev.timestamp_ns, _bucket_ns);
  const uint32_t key_sym = _by_symbol ? symbol_id : 0u;
  // Side encoding in result rows: 0 = aggregate, 1 = BUY, 2 = SELL.
  // Trade.side raw encoding (per execution/order_group.h): 0 = BUY,
  // 1 = SELL — shift by +1 to land in {1,2} when by_side=true.
  uint8_t key_side = 0;
  if (_by_side && is_trade)
  {
    key_side = static_cast<uint8_t>(raw_side == 0 ? 1 : 2);
  }

  ++_counts[std::tuple<int64_t, uint32_t, uint8_t>{bucket, key_sym, key_side}];
}

void BinCountAggregator::finalize()
{
  _rows.clear();
  _rows.reserve(_counts.size());
  for (const auto& [key, count] : _counts)
  {
    Row r;
    r.bucket_ts_ns = std::get<0>(key);
    r.symbol_id = std::get<1>(key);
    r.side = std::get<2>(key);
    r.count = count;
    _rows.push_back(r);
  }
  // _counts is std::map → already sorted by tuple order, which is
  // exactly (bucket_ts_ns, symbol_id, side). No extra sort needed.
}

std::unique_ptr<IAggregator> BinCountAggregator::cloneEmpty() const
{
  return std::make_unique<BinCountAggregator>(_bucket_ns, _by_side, _by_symbol,
                                              _event_filter, _symbol_filter);
}

void BinCountAggregator::merge(const IAggregator& other)
{
  const auto* o = dynamic_cast<const BinCountAggregator*>(&other);
  if (o == nullptr)
  {
    throw std::invalid_argument(
        "BinCountAggregator::merge: other is not the same concrete type");
  }
  for (const auto& [key, count] : o->_counts)
  {
    _counts[key] += count;
  }
}

}  // namespace lrvx::replay
