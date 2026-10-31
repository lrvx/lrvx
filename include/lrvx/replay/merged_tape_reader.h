/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/readers/binary_log_reader.h"
#include "lrvx/replay/recording_metadata.h"

#include <memory>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lrvx::replay
{

class IAggregator;

struct MergedTradeRow
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t price_raw{0};
  int64_t qty_raw{0};
  uint64_t trade_id{0};
  uint32_t global_symbol_id{0};
  uint32_t tape_index{0};
  uint8_t side{0};
};

struct MergedBookRow
{
  int64_t exchange_ts_ns{0};
  int64_t recv_ts_ns{0};
  int64_t seq{0};
  uint32_t global_symbol_id{0};
  uint32_t tape_index{0};
  uint16_t bid_count{0};
  uint16_t ask_count{0};
  uint64_t level_offset{0};
  uint8_t event_type{0};  // frame type: 2 = snapshot, 3 = delta, 6 = top-of-book
};

struct MergedSymbol
{
  uint32_t global_id{0};
  std::string exchange;
  std::string name;
  int8_t price_precision{8};
  int8_t qty_precision{8};
};

struct MergedTapeReaderConfig
{
  std::vector<std::filesystem::path> tape_dirs;
  std::optional<int64_t> from_ns;
  std::optional<int64_t> to_ns;
  // Filter applied post-rekey. Empty = all symbols.
  std::vector<uint32_t> symbol_filter;

  // Per-tape cross-block reorder window applied by `streamEvents` /
  // `run()`. Same semantics as `ReaderConfig::reorder_window_ns`: for
  // segments without the Sorted flag, each per-tape stream is
  // re-ordered through a bounded min-heap so the N-way merge sees
  // monotonic per-tape input. Events arriving past the window throw
  // LrvxError E_DATA_002. Memory bound: N_tapes × reorder_window_ns
  // × peak_event_rate × sizeof(ReplayEvent). Default matches
  // ReaderConfig (10s).
  int64_t reorder_window_ns{10'000'000'000};
};

class OverlappingBookStreamError : public std::runtime_error
{
 public:
  using std::runtime_error::runtime_error;
};

/// K-tape consumption primitive. Reads N `.lrvx` directories,
/// builds a global symbol registry by `(metadata.exchange, name)`
/// keying, and exposes the merged trade / book streams sorted by
/// exchange_ts_ns. Tie-break: (exchange_ts_ns, tape_index, seq) for books,
/// which carry a source sequence number, and (exchange_ts_ns, tape_index,
/// source order) for trades, which do not. Every venue that batches an update
/// prints several events on one nanosecond, so the tie is the normal case and
/// its resolution is what makes a replay reproducible.
///
/// V1 materialization: `read_trades` / `read_books` collect all events
/// across tapes, then sort. Memory bound = total events × row size.
/// A streaming heap-based iterator is a follow-up; the materialization
/// path keeps API parity with `DataReader`.
class MergedTapeReader
{
 public:
  explicit MergedTapeReader(MergedTapeReaderConfig config);

  MergedTapeReader(const MergedTapeReader&) = delete;
  MergedTapeReader& operator=(const MergedTapeReader&) = delete;

  // Global symbol registry, in global_id order.
  const std::vector<MergedSymbol>& symbols() const noexcept { return _symbols; }

  // Aggregate time range across all tapes (min first, max last).
  std::pair<int64_t, int64_t> timeRange() const noexcept { return _time_range; }

  // Aggregate summary across all tapes. Mirrors
  // `BinaryLogReader::DatasetSummary` for a single tape — total event
  // counts + time range + distinct (rekeyed) symbols + tape count.
  struct Summary
  {
    int64_t first_event_ns{0};
    int64_t last_event_ns{0};
    uint64_t total_events{0};
    uint32_t tape_count{0};
    uint32_t symbol_count{0};
    bool empty() const noexcept { return total_events == 0; }
  };
  Summary summary() const noexcept;

  // Merged sorted trade rows. Empty filter = all symbols.
  std::vector<MergedTradeRow> readTrades();

  // Merged sorted book headers + flat levels. levels are laid out as
  // bids then asks per event; each header's `level_offset` indexes into
  // the returned levels vector.
  std::pair<std::vector<MergedBookRow>, std::vector<BookLevel>> readBooks();

  // Streaming walk via N-way heap merge over per-tape iterators.
  // O(N tapes) peak memory regardless of total event count — the
  // path to take for long captures where `readTrades` / `readBooks`
  // would blow the heap budget.
  //
  // `callback` is invoked once per event in (exchange_ts_ns,
  // tape_index) order. Returning `false` aborts the walk. The
  // ReplayEvent has its `trade.symbol_id` / `book_header.symbol_id`
  // already rewritten to the global id.
  using StreamCallback = std::function<bool(uint32_t tape_index,
                                            const ReplayEvent& event)>;
  bool streamEvents(StreamCallback callback);

  // Single-pass streaming aggregator dispatch over the merged stream.
  // Walks via the same N-way heap merge as `streamEvents`, forwarding
  // each event (with global-rewritten symbol ids) to every aggregator's
  // onEvent, then calling finalize() on each. An empty span is a no-op
  // and performs no decompression. The `tape_index` of the originating
  // tape is not surfaced to aggregators — they see the merged stream
  // as one sequence; reach for `streamEvents` directly when per-tape
  // provenance matters.
  bool run(std::span<IAggregator* const> aggregators);

  // IMultiSegmentReader adapter — wraps `this` so the merged stream
  // plugs into anything that consumes a single-tape `IMultiSegmentReader`
  // (BacktestRunner::run, primarily). Lifetime of the returned pointer
  // is bounded by `this` MergedTapeReader.
  std::unique_ptr<IMultiSegmentReader> asMultiSegmentReader();

  // Per-tape ground-truth counts from the recording manifest. Populated
  // at construction so callers can audit "one tape is empty" without
  // a `readTrades`/`readBooks` round trip. Legacy tapes recorded before
  // `manifest.total_trades` / `total_book_updates` were added report
  // 0 here — for those, `summary()` falls back to the lumped
  // `BinaryLogReader::inspect` total so the aggregate stays useful.
  struct PerTapeStats
  {
    std::filesystem::path path;
    uint64_t trades{0};
    uint64_t books{0};
    int64_t first_event_ns{0};
    int64_t last_event_ns{0};
  };
  const std::vector<PerTapeStats>& perTapeStats() const noexcept
  {
    return _per_tape_stats;
  }

 private:
  void loadManifests();
  void detectBookOverlap();
  uint32_t globalIdForLocal(uint32_t tape_index, uint32_t local_id) const;
  bool symbolPassesFilter(uint32_t global_id) const;

  MergedTapeReaderConfig _config;

  std::vector<RecordingMetadata> _manifests;
  std::vector<MergedSymbol> _symbols;
  // Per tape: local_id (manifest entry) → global_id. -1 = unmapped.
  std::vector<std::vector<int64_t>> _local_to_global;
  std::vector<PerTapeStats> _per_tape_stats;
  // total_events from `BinaryLogReader::inspect` at construction time
  // — `_per_tape_stats[i].trades + .books` is only filled after a
  // `readTrades`/`readBooks` pass, so this is the only count available
  // for `summary()` on a freshly constructed reader.
  std::vector<uint64_t> _inspect_total_events;
  std::pair<int64_t, int64_t> _time_range{0, 0};
};

}  // namespace lrvx::replay
