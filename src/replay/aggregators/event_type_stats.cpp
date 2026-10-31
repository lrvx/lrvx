/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/replay/aggregators/event_type_stats.h"

#include <algorithm>
#include <stdexcept>

namespace lrvx::replay
{

EventTypeStatsAggregator::EventTypeStatsAggregator(AggregatorEventFilter event_filter,
                                                   std::vector<uint32_t> symbol_filter)
    : _event_filter(event_filter), _symbol_filter(std::move(symbol_filter))
{
  // Sort the symbol filter once so symbolAllowed can binary-search
  // instead of linear-scan per event.
  std::sort(_symbol_filter.begin(), _symbol_filter.end());
  _symbol_filter.erase(std::unique(_symbol_filter.begin(), _symbol_filter.end()),
                       _symbol_filter.end());
}

bool EventTypeStatsAggregator::symbolAllowed(uint32_t symbol_id) const noexcept
{
  if (_symbol_filter.empty())
  {
    return true;
  }
  return std::binary_search(_symbol_filter.begin(), _symbol_filter.end(), symbol_id);
}

void EventTypeStatsAggregator::onEvent(const ReplayEvent& ev)
{
  uint32_t symbol_id = 0;
  bool is_trade = false;
  bool is_snapshot = false;
  bool is_delta = false;

  switch (ev.type)
  {
    case EventType::Trade:
      if (_event_filter == AggregatorEventFilter::BooksOnly)
      {
        return;
      }
      symbol_id = ev.trade.symbol_id;
      is_trade = true;
      break;
    // BookTop lands in default: the overview promises the three original
    // kinds, and a new column is a C-struct change for every binding.
    case EventType::BookSnapshot:
      if (_event_filter == AggregatorEventFilter::Trades)
      {
        return;
      }
      symbol_id = ev.book_header.symbol_id;
      is_snapshot = true;
      break;
    case EventType::BookDelta:
      if (_event_filter == AggregatorEventFilter::Trades)
      {
        return;
      }
      symbol_id = ev.book_header.symbol_id;
      is_delta = true;
      break;
    default:
      return;
  }

  if (!symbolAllowed(symbol_id))
  {
    return;
  }

  auto& row = _counts[symbol_id];
  row.symbol_id = symbol_id;
  if (is_trade)
  {
    ++row.trades;
  }
  else if (is_snapshot)
  {
    ++row.book_snapshots;
  }
  else if (is_delta)
  {
    ++row.book_deltas;
  }
}

void EventTypeStatsAggregator::finalize()
{
  _rows.clear();
  _rows.reserve(_counts.size());
  for (const auto& [sid, row] : _counts)
  {
    _rows.push_back(row);
  }
  std::sort(_rows.begin(), _rows.end(),
            [](const PerSymbolRow& a, const PerSymbolRow& b)
            { return a.symbol_id < b.symbol_id; });
}

std::unique_ptr<IAggregator> EventTypeStatsAggregator::cloneEmpty() const
{
  return std::make_unique<EventTypeStatsAggregator>(_event_filter, _symbol_filter);
}

void EventTypeStatsAggregator::merge(const IAggregator& other)
{
  const auto* o = dynamic_cast<const EventTypeStatsAggregator*>(&other);
  if (o == nullptr)
  {
    throw std::invalid_argument(
        "EventTypeStatsAggregator::merge: other is not the same concrete type");
  }
  for (const auto& [sid, row] : o->_counts)
  {
    auto& mine = _counts[sid];
    mine.symbol_id = sid;
    mine.trades += row.trades;
    mine.book_snapshots += row.book_snapshots;
    mine.book_deltas += row.book_deltas;
  }
}

}  // namespace lrvx::replay
