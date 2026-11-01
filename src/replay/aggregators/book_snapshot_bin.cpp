/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/aggregators/book_snapshot_bin.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace lrvx::replay
{

namespace
{
int64_t bucketStart(int64_t ts_ns, int64_t bucket_ns)
{
  if (ts_ns >= 0)
  {
    return (ts_ns / bucket_ns) * bucket_ns;
  }
  int64_t q = ts_ns / bucket_ns;
  if (ts_ns % bucket_ns != 0)
  {
    --q;
  }
  return q * bucket_ns;
}
}  // namespace

BookSnapshotBinAggregator::BookSnapshotBinAggregator(
    int64_t bucket_ns, uint16_t levels, AggregatorEventFilter event_filter,
    std::vector<uint32_t> symbol_filter)
    : _bucket_ns(bucket_ns),
      _levels(levels),
      _event_filter(event_filter),
      _symbol_filter(std::move(symbol_filter))
{
  if (bucket_ns <= 0)
  {
    throw std::invalid_argument("BookSnapshotBinAggregator: bucket_ns must be > 0");
  }
  if (levels == 0)
  {
    throw std::invalid_argument("BookSnapshotBinAggregator: levels must be > 0");
  }
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool BookSnapshotBinAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void BookSnapshotBinAggregator::emitCell(uint32_t symbol_id, const SymState& st)
{
  if (st.bids.empty() && st.asks.empty())
  {
    return;
  }
  uint16_t flags = 0;
  if (!st.bids.empty() && !st.asks.empty() &&
      st.bids.begin()->first >= st.asks.begin()->first)
  {
    flags |= kFlagCrossed;
  }
  auto bid_it = st.bids.begin();
  auto ask_it = st.asks.begin();
  for (uint16_t lvl = 0; lvl < _levels; ++lvl)
  {
    const bool has_bid = bid_it != st.bids.end();
    const bool has_ask = ask_it != st.asks.end();
    if (!has_bid && !has_ask)
    {
      break;
    }
    Row r;
    r.bucket_ts_ns = st.cur_bucket;
    r.symbol_id = symbol_id;
    r.level = lvl;
    r.flags = flags;
    if (has_bid)
    {
      r.bid_price_raw = bid_it->first;
      r.bid_qty_raw = bid_it->second;
      ++bid_it;
    }
    if (has_ask)
    {
      r.ask_price_raw = ask_it->first;
      r.ask_qty_raw = ask_it->second;
      ++ask_it;
    }
    _rows.push_back(r);
  }
}

void BookSnapshotBinAggregator::onEvent(const ReplayEvent& ev)
{
  // BookTop is skipped on purpose: it carries the touch, not book depth,
  // and this binner reconstructs depth from snapshots and deltas.
  if (ev.type != EventType::BookSnapshot && ev.type != EventType::BookDelta)
  {
    return;
  }
  if (_event_filter == AggregatorEventFilter::Trades)
  {
    return;
  }
  if (!symbolAllowed(ev.book_header.symbol_id))
  {
    return;
  }

  const uint32_t sym = ev.book_header.symbol_id;
  const int64_t bucket = bucketStart(ev.timestamp_ns, _bucket_ns);
  SymState& st = _states[sym];

  // Crossing a bucket boundary closes the prior bucket BEFORE this
  // event mutates the ladder, so the emitted rows reflect the state
  // at bucket end (exclusive of the first event of the next bucket).
  if (st.has_bucket && bucket > st.cur_bucket)
  {
    emitCell(sym, st);
  }
  if (!st.has_bucket || bucket > st.cur_bucket)
  {
    st.cur_bucket = bucket;
    st.has_bucket = true;
  }

  if (ev.type == EventType::BookSnapshot)
  {
    st.bids.clear();
    st.asks.clear();
  }
  for (const BookLevel& lvl : ev.bids)
  {
    if (lvl.qty_raw == 0)
    {
      st.bids.erase(lvl.price_raw);
    }
    else
    {
      st.bids[lvl.price_raw] = lvl.qty_raw;
    }
  }
  for (const BookLevel& lvl : ev.asks)
  {
    if (lvl.qty_raw == 0)
    {
      st.asks.erase(lvl.price_raw);
    }
    else
    {
      st.asks[lvl.price_raw] = lvl.qty_raw;
    }
  }
}

void BookSnapshotBinAggregator::finalize()
{
  for (const auto& [sym, st] : _states)
  {
    if (st.has_bucket)
    {
      emitCell(sym, st);
    }
  }
  std::stable_sort(_rows.begin(), _rows.end(),
                   [](const Row& a, const Row& b)
                   {
                     if (a.bucket_ts_ns != b.bucket_ts_ns)
                     {
                       return a.bucket_ts_ns < b.bucket_ts_ns;
                     }
                     if (a.symbol_id != b.symbol_id)
                     {
                       return a.symbol_id < b.symbol_id;
                     }
                     return a.level < b.level;
                   });
}

std::unique_ptr<IAggregator> BookSnapshotBinAggregator::cloneEmpty() const
{
  throw std::runtime_error(
      "BookSnapshotBinAggregator: book reconstruction is order-dependent across "
      "the whole tape and cannot run on partitioned workers; use "
      "DataReader.run(..., n_threads=1)");
}

void BookSnapshotBinAggregator::merge(const IAggregator&)
{
  throw std::runtime_error(
      "BookSnapshotBinAggregator: merge of partial book states is undefined; use "
      "DataReader.run(..., n_threads=1)");
}

}  // namespace lrvx::replay
