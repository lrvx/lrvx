/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/common.h"
#include "lrvx/log/log.h"
#include "lrvx/util/base/time.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lrvx
{

// One row of a per-symbol funding-rate tape. A `symbol` value of
// `kAnySymbol` (0) means "applies to every symbol at this timestamp"
// — convenient when an archive carries a single rate for the venue.
struct FundingTapeEntry
{
  static constexpr SymbolId kAnySymbol = 0;
  UnixNanos timestampNs{};  // wall epoch: funding archives are venue history
  SymbolId symbol{kAnySymbol};
  double rate{0.0};
};

// One funding payment as recorded by FundingSchedule::tick.
//
// `amount` is signed: positive means the position holder received
// the payment, negative means they paid it. Sign follows the
// convention long pays positive funding (rate>0), short pays
// negative funding.
struct FundingPayment
{
  UnixNanos timestampNs{};  // wall epoch: stamped from the (simulated) wall clock
  SymbolId symbol{};
  double rate{0.0};
  double markPrice{0.0};
  double positionSigned{0.0};  // signed position (long = +)
  double amount{0.0};          // settled value in margin currency
};

// Funding schedule. Two construction modes:
//
//   constant(interval_ns, rate)
//       Fixed-interval, constant-rate model. Useful for stress tests
//       and quick parametric runs. The rate is applied every
//       interval_ns boundary.
//
//   tape(events)
//       Point-in-time recorded rates from an exchange archive. Each
//       (timestamp_ns, rate) pair triggers one payment at that ts.
//
// Canned profiles match the published intervals for major venues
// (Binance UM 8h, Bybit linear 8h, OKX 8h, Bitget some markets 1h).
// The rate value in canned profiles is zero — callers must supply
// a real rate via a tape or override.
class FundingSchedule
{
 public:
  FundingSchedule() = default;

  static FundingSchedule constant(int64_t intervalNs, double rate)
  {
    FundingSchedule s;
    s._intervalNs = intervalNs;
    s._constantRate = rate;
    return s;
  }

  static FundingSchedule tape(std::vector<std::pair<int64_t, double>> events)
  {
    FundingSchedule s;
    s._tape = std::move(events);
    std::sort(s._tape.begin(), s._tape.end(),
              [](const auto& a, const auto& b)
              { return a.first < b.first; });
    s.rebuildPerSymbolFromLegacyTape();
    return s;
  }

  // Per-symbol tape: each (ts, symbol, rate) row settles only the
  // matching symbol at that timestamp. Symbols not represented at a
  // given settlement fall back to the constant rate (which is 0 by
  // default; override with setConstantRate).
  static FundingSchedule tapeBySymbol(std::vector<FundingTapeEntry> entries)
  {
    FundingSchedule s;
    s._perSymbolTape = std::move(entries);
    s.rebuildSettlementsFromPerSymbolTape();
    return s;
  }

  // Parse a CSV with columns timestamp_ns, symbol, funding_rate (in
  // that order). Lines starting with '#' or matching the header are
  // skipped. Whitespace is trimmed. Returns true on success; false if
  // the file could not be opened (existing tape state preserved on
  // error).
  bool loadTape(const std::string& path)
  {
    std::ifstream in(path);
    if (!in.is_open())
    {
      return false;
    }
    std::vector<FundingTapeEntry> rows;
    std::string line;
    while (std::getline(in, line))
    {
      if (line.empty() || line[0] == '#')
      {
        continue;
      }
      // Header sniff: first non-empty, non-comment line containing
      // alphabetic chars but no digits.
      bool isHeader = false;
      if (rows.empty())
      {
        bool hasAlpha = false;
        bool hasDigit = false;
        for (char c : line)
        {
          if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
          {
            hasAlpha = true;
          }
          else if (c >= '0' && c <= '9')
          {
            hasDigit = true;
          }
        }
        if (hasAlpha && !hasDigit)
        {
          isHeader = true;
        }
      }
      if (isHeader)
      {
        continue;
      }
      std::stringstream ss(line);
      std::string tsStr;
      std::string symStr;
      std::string rateStr;
      if (!std::getline(ss, tsStr, ',') || !std::getline(ss, symStr, ',') ||
          !std::getline(ss, rateStr, ','))
      {
        continue;
      }
      try
      {
        FundingTapeEntry e;
        e.timestampNs = UnixNanos::fromRaw(std::stoll(tsStr));
        e.symbol = static_cast<SymbolId>(std::stoul(symStr));
        e.rate = std::stod(rateStr);
        rows.push_back(e);
      }
      catch (...)
      {
        // Malformed line — skip.
      }
    }
    _perSymbolTape = std::move(rows);
    _intervalNs = 0;
    rebuildSettlementsFromPerSymbolTape();
    return true;
  }

  static FundingSchedule binance_um_futures()
  {
    // 8-hour fixed cadence, default rate zero (caller overrides).
    return constant(8LL * 3600LL * 1'000'000'000LL, 0.0);
  }

  static FundingSchedule bybit_linear()
  {
    return constant(8LL * 3600LL * 1'000'000'000LL, 0.0);
  }

  static FundingSchedule okx_swap() { return binance_um_futures(); }

  static FundingSchedule bitget_hourly()
  {
    return constant(3600LL * 1'000'000'000LL, 0.0);
  }

  bool isInterval() const noexcept { return _intervalNs > 0; }
  int64_t intervalNs() const noexcept { return _intervalNs; }
  double constantRate() const noexcept { return _constantRate; }
  const std::vector<std::pair<int64_t, double>>& tapeEvents() const noexcept
  {
    return _tape;
  }
  const std::vector<FundingTapeEntry>& perSymbolTape() const noexcept
  {
    return _perSymbolTape;
  }
  // Settlement timestamps in the per-symbol tape — unique, sorted.
  const std::vector<int64_t>& settlementTimestamps() const noexcept
  {
    return _settlementTimestamps;
  }

  void setConstantRate(double rate) noexcept { _constantRate = rate; }

  // Walk every funding event in (lastTickNs, nowNs] and emit a
  // FundingPayment for each (symbol, position, mark) triple. The
  // caller supplies the current per-symbol position and mark price;
  // the schedule does the timing + math:
  //
  //   amount = -position_signed * mark * rate
  //
  // Sign convention: long (positive position) pays out when rate is
  // positive, so amount is negative for the long; short receives
  // (amount positive). Update internal lastTickNs so subsequent
  // calls do not re-emit the same events.
  std::vector<FundingPayment> tick(int64_t nowNs,
                                   const std::vector<SymbolId>& symbols,
                                   const std::vector<double>& positions,
                                   const std::vector<double>& markPrices)
  {
    std::vector<FundingPayment> out;

    // A schedule that has never ticked sits at _lastTickNs == 0, i.e. the
    // epoch. The first tick of a run stamped with real exchange timestamps
    // would therefore settle every boundary since 1970 against the position
    // held on that one tick: at an 8h cadence a 2026 timestamp is 61,362
    // payments. Seed the cursor from the first timestamp the run actually
    // shows us, so the opening tick settles at most the boundary that just
    // passed and the cadence afterwards is unchanged.
    //
    // std::max keeps a schedule driven from a small base (the unit tests, and
    // any run whose clock starts near zero) exactly where it was: there the
    // previous boundary is at or before the cursor, so nothing moves.
    if (_intervalNs > 0 && !_seeded)
    {
      _seeded = true;
      const int64_t previousBoundary = (nowNs / _intervalNs) * _intervalNs;
      _lastTickNs = std::max(_lastTickNs, previousBoundary - _intervalNs);
    }

    // Later ticks can still be handed a jump (a resumed run, a gap in the
    // tape). Warn and clamp rather than emitting an unbounded payment list.
    if (_intervalNs > 0)
    {
      const int64_t span = nowNs - _lastTickNs;
      if (span / _intervalNs > kMaxBoundariesPerTick)
      {
        LRVX_LOG_WARN("[FundingSchedule] tick would cross "
                      << (span / _intervalNs) << " boundaries; clamping to the "
                      << "last " << kMaxBoundariesPerTick);
        _lastTickNs = nowNs - kMaxBoundariesPerTick * _intervalNs;
      }
    }

    if (_intervalNs > 0)
    {
      // Fixed-interval mode: emit every boundary in (lastTick, now].
      const int64_t firstBoundary = ((_lastTickNs / _intervalNs) + 1) * _intervalNs;
      for (int64_t t = firstBoundary; t <= nowNs; t += _intervalNs)
      {
        if (t <= _lastTickNs)
        {
          continue;
        }
        for (size_t i = 0; i < symbols.size(); ++i)
        {
          if (positions[i] == 0.0)
          {
            continue;
          }
          FundingPayment p;
          p.timestampNs = UnixNanos::fromRaw(t);
          p.symbol = symbols[i];
          p.rate = _constantRate;
          p.markPrice = markPrices[i];
          p.positionSigned = positions[i];
          p.amount = -positions[i] * markPrices[i] * _constantRate;
          out.push_back(p);
        }
      }
    }
    else
    {
      // Tape mode: walk settlement timestamps. At each ts, every
      // symbol with an open position gets a payment; the rate is
      // looked up per-(symbol, ts) with fallback to the constant rate.
      for (int64_t ts : _settlementTimestamps)
      {
        if (ts <= _lastTickNs || ts > nowNs)
        {
          continue;
        }
        for (size_t i = 0; i < symbols.size(); ++i)
        {
          if (positions[i] == 0.0)
          {
            continue;
          }
          const double rate = rateFor(symbols[i], ts);
          FundingPayment p;
          p.timestampNs = UnixNanos::fromRaw(ts);
          p.symbol = symbols[i];
          p.rate = rate;
          p.markPrice = markPrices[i];
          p.positionSigned = positions[i];
          p.amount = -positions[i] * markPrices[i] * rate;
          out.push_back(p);
        }
      }
    }
    _lastTickNs = nowNs;
    return out;
  }

  // Reset the internal cursor — useful when restarting a backtest.
  void reset() noexcept
  {
    _lastTickNs = 0;
    _seeded = false;
  }
  int64_t lastTickNs() const noexcept { return _lastTickNs; }

  // Where tick() has got to. A caller that must size a buffer before it can
  // take the payments ticks, counts, and puts the cursor back.
  struct Cursor
  {
    int64_t lastTickNs{0};
    bool seeded{false};
  };
  Cursor cursor() const noexcept { return {_lastTickNs, _seeded}; }
  void restoreCursor(Cursor c) noexcept
  {
    _lastTickNs = c.lastTickNs;
    _seeded = c.seeded;
  }

 private:
  int64_t _intervalNs{0};
  double _constantRate{0.0};
  std::vector<std::pair<int64_t, double>> _tape;
  std::vector<FundingTapeEntry> _perSymbolTape;
  std::vector<int64_t> _settlementTimestamps;
  int64_t _lastTickNs{0};
  bool _seeded{false};  // first tick seeds the cursor from `now` (see tick())
  // Backstop for a jump on a later tick. ~1 year of 8h funding is 1095
  // settlements, so 100k is far above any window a run legitimately crosses
  // in one call; the epoch walk the first tick used to make is now handled by
  // seeding, not by this clamp.
  static constexpr int64_t kMaxBoundariesPerTick = 100'000;

  // Resolve rate for a (symbol, ts) pair. Search per-symbol tape for
  // an exact match; fall back to a wildcard (symbol == kAnySymbol)
  // entry at that ts; fall back to the constant rate.
  double rateFor(SymbolId symbol, int64_t ts) const noexcept
  {
    double wildcard = _constantRate;
    bool wildcardSet = false;
    for (const auto& e : _perSymbolTape)
    {
      if (e.timestampNs.raw() != ts)
      {
        continue;
      }
      if (e.symbol == symbol)
      {
        return e.rate;
      }
      if (e.symbol == FundingTapeEntry::kAnySymbol)
      {
        wildcard = e.rate;
        wildcardSet = true;
      }
    }
    (void)wildcardSet;
    return wildcard;
  }

  void rebuildSettlementsFromPerSymbolTape()
  {
    std::sort(_perSymbolTape.begin(), _perSymbolTape.end(),
              [](const FundingTapeEntry& a, const FundingTapeEntry& b)
              { return a.timestampNs < b.timestampNs; });
    _settlementTimestamps.clear();
    for (const auto& e : _perSymbolTape)
    {
      if (_settlementTimestamps.empty() ||
          _settlementTimestamps.back() != e.timestampNs.raw())
      {
        _settlementTimestamps.push_back(e.timestampNs.raw());
      }
    }
  }

  // For backwards compatibility with the legacy tape(events) API: map
  // each (ts, rate) into a per-symbol entry with symbol == kAnySymbol.
  void rebuildPerSymbolFromLegacyTape()
  {
    _perSymbolTape.clear();
    _perSymbolTape.reserve(_tape.size());
    for (const auto& [ts, rate] : _tape)
    {
      FundingTapeEntry e;
      e.timestampNs = UnixNanos::fromRaw(ts);
      e.symbol = FundingTapeEntry::kAnySymbol;
      e.rate = rate;
      _perSymbolTape.push_back(e);
    }
    rebuildSettlementsFromPerSymbolTape();
  }
};

}  // namespace lrvx
