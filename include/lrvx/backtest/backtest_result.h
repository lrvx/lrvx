/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/clearing/fee_schedule.h"
#include "lrvx/common.h"
#include "lrvx/util/base/time.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace lrvx
{

struct TradeRecord
{
  SymbolId symbol{};
  Side side{};
  Price entryPrice{};
  Price exitPrice{};
  Quantity quantity{};
  UnixNanos entryTimeNs{0};
  UnixNanos exitTimeNs{0};
  Volume pnl{};
  Volume fee{};
};

struct EquityPoint
{
  UnixNanos timestampNs{0};
  double equity{0.0};
  double drawdownPct{0.0};
};

struct BacktestStats
{
  size_t totalTrades{0};
  size_t winningTrades{0};
  size_t losingTrades{0};

  double initialCapital{0.0};
  double finalCapital{0.0};
  double totalPnl{0.0};
  double totalFees{0.0};
  double netPnl{0.0};
  double grossProfit{0.0};
  double grossLoss{0.0};

  double maxDrawdown{0.0};
  double maxDrawdownPct{0.0};

  double winRate{0.0};
  double profitFactor{0.0};
  double avgWin{0.0};
  double avgLoss{0.0};
  double avgWinLossRatio{0.0};

  size_t maxConsecutiveWins{0};
  size_t maxConsecutiveLosses{0};

  double avgTradeDurationNs{0.0};
  double medianTradeDurationNs{0.0};
  double maxTradeDurationNs{0.0};

  double sharpeRatio{0.0};
  double sortinoRatio{0.0};
  double calmarRatio{0.0};
  double timeWeightedReturn{0.0};
  double returnPct{0.0};

  UnixNanos startTimeNs{0};
  UnixNanos endTimeNs{0};

  // The positions still open when the run ended, each valued at its mark
  // (see BacktestResult::markPrice): (mark - entry) * qty for a long,
  // (entry - mark) * qty for a short, no exit fee. Nothing above includes
  // it -- netPnl, finalCapital and returnPct stay the closed trades' figures.
  double unrealizedPnl{0.0};
  // initialCapital + netPnl + unrealizedPnl.
  double finalEquity{0.0};
};

// A position the run had not closed when it ended.
struct OpenPosition
{
  SymbolId symbol{};
  Side side{};          // BUY for a long, SELL for a short
  Quantity quantity{};  // absolute
  Price entryPrice{};   // average entry
  Price markPrice{};
  Volume unrealizedPnl{};
};

class BacktestResult
{
 public:
  static constexpr size_t kMaxSymbols = 256;

  explicit BacktestResult(const BacktestConfig& config = {}, size_t expectedFills = 0);

  void recordFill(const Fill& fill);
  BacktestStats computeStats() const;

  // Price every fill through a venue's volume ladder instead of the flat
  // BacktestConfig::feeRate. The schedule is copied, and the copy is unbound
  // from whatever account drove it: the replay pushes the run's own notional
  // into that copy, so the tier climbs as the run trades while the venue's
  // live 30-day counter is left alone. Two results built from the same fill
  // stream therefore price it identically.
  //
  // Precedence: an explicit non-percentage fee model (usePercentageFee ==
  // false) still wins, because that is the caller replacing the fee model
  // outright. Otherwise the ladder outranks feeRate and the per-side
  // overrides -- a run on a venue pays what that venue charges.
  void setFeeSchedule(const FeeSchedule& schedule);

  const BacktestConfig& config() const { return _config; }

  const std::vector<Fill>& fills() const { return _fills; }
  const std::vector<TradeRecord>& trades() const { return _trades; }
  const std::vector<EquityPoint>& equityCurve() const { return _equityCurve; }
  double totalPnl() const;

  // The price an open position in `symbol` is valued at; the last call wins.
  // A symbol never marked is valued at its last fill. The backtest runners
  // mark every price they replay, so their results are marked at the run's
  // last price per symbol.
  void markPrice(SymbolId symbol, Price price);
  void markPrice(SymbolId symbol, double price) { markPrice(symbol, Price::fromDouble(price)); }
  // Every symbol with a position open, ascending symbol id.
  std::vector<OpenPosition> openPositions() const;

  // Writes timestamp_ns,equity,drawdown_pct CSV. Returns true on success.
  bool writeEquityCurveCsv(const std::string& path) const;

 private:
  struct Position
  {
    Quantity quantity{};
    Price avgPrice{};
    UnixNanos entryTimeNs{0};  // set when position opens from flat
    Volume entryFeeAcc{};      // fees accumulated on opens/adds since position opened
    Price lastFillPrice{};
    Price mark{};
    bool marked{false};
  };

  Position& getPosition(SymbolId symbol);
  static OpenPosition valueOpen(SymbolId symbol, const Position& pos);
  Volume unrealizedPnl() const;

  static Volume computePnl(Price entryPrice, Price exitPrice, Quantity qty, bool isLong);

  void updatePositionLong(Position& pos, Quantity qty, Price price, UnixNanos timestampNs);
  void updatePositionShort(Position& pos, Quantity qty, Price price, UnixNanos timestampNs);
  void recordTrade(SymbolId symbol, Side side, Price entryPrice, Price exitPrice,
                   Quantity quantity, UnixNanos entryTimeNs, UnixNanos exitTimeNs,
                   Volume pnl, Volume fee);
  // Not const: with a fee ladder attached this advances the replay's own
  // 30-day window, so the next fill resolves against the tier this one
  // helped reach.
  Volume computeFee(Price price, Quantity qty, bool isMaker, UnixNanos tsNs);

  // Ratios are computed from per-period returns derived from the equity curve.
  // Each return is (equity[i] - equity[i-1]) / equity[i-1] with the configured
  // riskFreeRate subtracted. Sharpe/Sortino are annualized by
  // sqrt(metricsAnnualizationFactor). Calmar annualizes the cumulative TWR
  // by the observed sample count and divides by the drawdown fraction; it
  // returns 0.0 below a minimum sample count rather than raising a short
  // curve's return to a huge power (see computeCalmarRatio).
  double computeSharpeRatio() const;
  double computeSortinoRatio() const;
  double computeCalmarRatio(double cumulativeTwr) const;
  double computeTimeWeightedReturn() const;

  BacktestConfig _config;

  std::vector<Fill> _fills;
  std::vector<TradeRecord> _trades;
  std::vector<EquityPoint> _equityCurve;

  std::array<Position, kMaxSymbols> _positionsFlat{};
  std::vector<std::pair<SymbolId, Position>> _positionsOverflow;

  Volume _totalPnl{};
  Volume _totalFees{};
  Volume _currentEquity{};
  Volume _peakEquity{};
  Volume _maxDrawdown{};
  // Captured against the peak that was standing when the drawdown happened.
  // Dividing the absolute maximum by the peak equity as it stands at the end
  // of the run understates risk by however much the account grew afterwards:
  // a 1M to 0.7M to 3M curve reported 10% against a true 30%, and the Calmar
  // ratio built on the same denominator was inflated by the same factor.
  double _maxDrawdownPct{0.0};

  // Venue fee ladder, if one was attached. `_feeBaseNotional30d` is the
  // 30-day notional the venue's account already carried when the schedule
  // was attached; it is pushed into the copy on the first fill, where there
  // is finally a timestamp to stamp it with.
  std::optional<FeeSchedule> _fees{};
  double _feeBaseNotional30d{0.0};
  bool _feeBaseApplied{false};
};

}  // namespace lrvx
