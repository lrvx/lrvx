/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/backtest/backtest_result.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

namespace lrvx
{

BacktestResult::BacktestResult(const BacktestConfig& config, size_t expectedFills) : _config(config)
{
  if (expectedFills > 0)
  {
    _fills.reserve(expectedFills);
    _trades.reserve(expectedFills / 2);
    _equityCurve.reserve(expectedFills / 2);
  }
  _currentEquity = Volume::fromDouble(_config.initialCapital);
  _peakEquity = _currentEquity;
}

void BacktestResult::recordFill(const Fill& fill)
{
  _fills.push_back(fill);

  const Volume fee = computeFee(fill.price, fill.quantity, fill.isMaker, fill.timestampNs);
  _totalFees = Volume::fromRaw(_totalFees.raw() + fee.raw());

  Position& pos = getPosition(fill.symbol);
  pos.lastFillPrice = fill.price;

  // Pro-rate this fill's fee between a close portion and a new-open portion
  // by quantity, so a flipping fill (close + open) attributes fees correctly
  // to the closed trade and the new position.
  //
  // The product runs at full width. Written out in int64 it passes the ceiling
  // once the per-side fee in quote currency times the quantity passes about
  // 922, which a 7 BTC round trip at four basis points already does, and the
  // wrap handed back a NEGATIVE per-trade fee. Equity after a flat round trip
  // then sat above where it started, while the totals in the same object
  // stayed correct, so no summary line showed the split.
  auto feeFractionRaw = [&](const Quantity& part) -> int64_t
  {
    if (fill.quantity.raw() == 0)
    {
      return 0;
    }
    return mulDivI64(fee.raw(), part.raw(), fill.quantity.raw());
  };

  if (fill.side == Side::BUY)
  {
    if (pos.quantity.raw() < 0)
    {
      const Quantity closeQty =
          Quantity::fromRaw(std::min(-pos.quantity.raw(), fill.quantity.raw()));
      const Volume pnl = computePnl(pos.avgPrice, fill.price, closeQty, false);
      const int64_t entryFeePortionRaw =
          (pos.quantity.raw() != 0)
              ? mulDivI64(pos.entryFeeAcc.raw(), closeQty.raw(), -pos.quantity.raw())
              : 0;
      const Volume tradeFee =
          Volume::fromRaw(entryFeePortionRaw + feeFractionRaw(closeQty));
      recordTrade(fill.symbol, Side::BUY, pos.avgPrice, fill.price, closeQty,
                  pos.entryTimeNs, fill.timestampNs, pnl, tradeFee);
      pos.entryFeeAcc = Volume::fromRaw(pos.entryFeeAcc.raw() - entryFeePortionRaw);
      const Quantity openQty =
          Quantity::fromRaw(fill.quantity.raw() - closeQty.raw());
      if (openQty.raw() > 0)
      {
        pos.entryFeeAcc =
            Volume::fromRaw(pos.entryFeeAcc.raw() + feeFractionRaw(openQty));
      }
    }
    else
    {
      pos.entryFeeAcc = Volume::fromRaw(pos.entryFeeAcc.raw() + fee.raw());
    }
    updatePositionLong(pos, fill.quantity, fill.price, fill.timestampNs);
  }
  else
  {
    if (pos.quantity.raw() > 0)
    {
      const Quantity closeQty =
          Quantity::fromRaw(std::min(pos.quantity.raw(), fill.quantity.raw()));
      const Volume pnl = computePnl(pos.avgPrice, fill.price, closeQty, true);
      const int64_t entryFeePortionRaw =
          (pos.quantity.raw() != 0)
              ? mulDivI64(pos.entryFeeAcc.raw(), closeQty.raw(), pos.quantity.raw())
              : 0;
      const Volume tradeFee =
          Volume::fromRaw(entryFeePortionRaw + feeFractionRaw(closeQty));
      recordTrade(fill.symbol, Side::SELL, pos.avgPrice, fill.price, closeQty,
                  pos.entryTimeNs, fill.timestampNs, pnl, tradeFee);
      pos.entryFeeAcc = Volume::fromRaw(pos.entryFeeAcc.raw() - entryFeePortionRaw);
      const Quantity openQty =
          Quantity::fromRaw(fill.quantity.raw() - closeQty.raw());
      if (openQty.raw() > 0)
      {
        pos.entryFeeAcc =
            Volume::fromRaw(pos.entryFeeAcc.raw() + feeFractionRaw(openQty));
      }
    }
    else
    {
      pos.entryFeeAcc = Volume::fromRaw(pos.entryFeeAcc.raw() + fee.raw());
    }
    updatePositionShort(pos, fill.quantity, fill.price, fill.timestampNs);
  }

  // When the position closes fully, reset the residual entry-fee accumulator.
  if (pos.quantity.raw() == 0)
  {
    pos.entryFeeAcc = Volume::fromRaw(0);
  }
}

BacktestStats BacktestResult::computeStats() const
{
  BacktestStats stats;
  stats.totalTrades = _trades.size();
  stats.initialCapital = _config.initialCapital;
  stats.unrealizedPnl = unrealizedPnl().toDouble();

  if (stats.totalTrades == 0)
  {
    stats.finalCapital = _config.initialCapital;
    stats.finalEquity = _config.initialCapital + stats.unrealizedPnl;
    return stats;
  }

  int64_t grossProfitRaw = 0;
  int64_t grossLossRaw = 0;
  size_t winStreak = 0;
  size_t lossStreak = 0;
  std::vector<double> durations;
  durations.reserve(_trades.size());

  for (const auto& trade : _trades)
  {
    if (trade.pnl.raw() > 0)
    {
      stats.winningTrades++;
      grossProfitRaw += trade.pnl.raw();
      ++winStreak;
      lossStreak = 0;
      if (winStreak > stats.maxConsecutiveWins)
      {
        stats.maxConsecutiveWins = winStreak;
      }
    }
    else if (trade.pnl.raw() < 0)
    {
      stats.losingTrades++;
      grossLossRaw += -trade.pnl.raw();
      ++lossStreak;
      winStreak = 0;
      if (lossStreak > stats.maxConsecutiveLosses)
      {
        stats.maxConsecutiveLosses = lossStreak;
      }
    }
    else
    {
      winStreak = 0;
      lossStreak = 0;
    }

    if (trade.exitTimeNs >= trade.entryTimeNs)
    {
      durations.push_back(
          static_cast<double>((trade.exitTimeNs - trade.entryTimeNs).count()));
    }
  }

  stats.totalPnl = _totalPnl.toDouble();
  stats.totalFees = _totalFees.toDouble();
  stats.netPnl = stats.totalPnl - stats.totalFees;
  stats.grossProfit = Volume::fromRaw(grossProfitRaw).toDouble();
  stats.grossLoss = Volume::fromRaw(grossLossRaw).toDouble();

  stats.finalCapital = _config.initialCapital + stats.netPnl;
  stats.finalEquity = stats.finalCapital + stats.unrealizedPnl;
  stats.returnPct = (stats.netPnl / _config.initialCapital) * 100.0;

  stats.winRate =
      static_cast<double>(stats.winningTrades) / static_cast<double>(stats.totalTrades);

  if (stats.winningTrades > 0)
  {
    stats.avgWin = stats.grossProfit / static_cast<double>(stats.winningTrades);
  }

  if (stats.losingTrades > 0)
  {
    stats.avgLoss = stats.grossLoss / static_cast<double>(stats.losingTrades);
  }

  if (stats.avgLoss > 0.0)
  {
    stats.avgWinLossRatio = stats.avgWin / stats.avgLoss;
  }

  if (grossLossRaw > 0)
  {
    stats.profitFactor = static_cast<double>(grossProfitRaw) / static_cast<double>(grossLossRaw);
  }

  stats.maxDrawdown = _maxDrawdown.toDouble();
  // Against the peak that stood at the time, the same number the equity curve
  // reports point by point. The two used to be computed differently and the
  // backtest report printed both on one page.
  stats.maxDrawdownPct = _maxDrawdownPct;

  if (!durations.empty())
  {
    double sum = 0.0;
    double maxDur = 0.0;
    for (double d : durations)
    {
      sum += d;
      if (d > maxDur)
      {
        maxDur = d;
      }
    }
    stats.avgTradeDurationNs = sum / static_cast<double>(durations.size());
    stats.maxTradeDurationNs = maxDur;
    std::vector<double> sorted = durations;
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    stats.medianTradeDurationNs = (sorted.size() % 2 == 0)
                                      ? 0.5 * (sorted[mid - 1] + sorted[mid])
                                      : sorted[mid];
  }

  stats.sharpeRatio = computeSharpeRatio();
  stats.sortinoRatio = computeSortinoRatio();
  stats.timeWeightedReturn = computeTimeWeightedReturn();
  stats.calmarRatio = computeCalmarRatio(stats.timeWeightedReturn);

  if (!_fills.empty())
  {
    stats.startTimeNs = _fills.front().timestampNs;
    stats.endTimeNs = _fills.back().timestampNs;
  }

  return stats;
}

double BacktestResult::totalPnl() const
{
  return _totalPnl.toDouble();
}

void BacktestResult::markPrice(SymbolId symbol, Price price)
{
  Position& pos = getPosition(symbol);
  pos.mark = price;
  pos.marked = true;
}

OpenPosition BacktestResult::valueOpen(SymbolId symbol, const Position& pos)
{
  const bool isLong = pos.quantity.raw() > 0;
  const Quantity absQty = Quantity::fromRaw(isLong ? pos.quantity.raw() : -pos.quantity.raw());
  OpenPosition out;
  out.symbol = symbol;
  out.side = isLong ? Side::BUY : Side::SELL;
  out.quantity = absQty;
  out.entryPrice = pos.avgPrice;
  out.markPrice = pos.marked ? pos.mark : pos.lastFillPrice;
  out.unrealizedPnl = computePnl(pos.avgPrice, out.markPrice, absQty, isLong);
  return out;
}

std::vector<OpenPosition> BacktestResult::openPositions() const
{
  std::vector<OpenPosition> out;
  for (size_t sym = 0; sym < kMaxSymbols; ++sym)
  {
    if (_positionsFlat[sym].quantity.raw() != 0)
    {
      out.push_back(valueOpen(static_cast<SymbolId>(sym), _positionsFlat[sym]));
    }
  }
  const size_t flatCount = out.size();
  for (const auto& [id, pos] : _positionsOverflow)
  {
    if (pos.quantity.raw() != 0)
    {
      out.push_back(valueOpen(id, pos));
    }
  }
  std::sort(out.begin() + static_cast<std::ptrdiff_t>(flatCount), out.end(),
            [](const OpenPosition& a, const OpenPosition& b)
            { return a.symbol < b.symbol; });
  return out;
}

Volume BacktestResult::unrealizedPnl() const
{
  int64_t raw = 0;
  for (const auto& p : openPositions())
  {
    raw += p.unrealizedPnl.raw();
  }
  return Volume::fromRaw(raw);
}

bool BacktestResult::writeEquityCurveCsv(const std::string& path) const
{
  std::ofstream out(path);
  if (!out)
  {
    return false;
  }
  out << "timestamp_ns,equity,drawdown_pct\n";
  for (const auto& p : _equityCurve)
  {
    out << p.timestampNs.raw() << ',' << p.equity << ','
        << p.drawdownPct << '\n';
  }
  return static_cast<bool>(out);
}

BacktestResult::Position& BacktestResult::getPosition(SymbolId symbol)
{
  if (symbol < kMaxSymbols) [[likely]]
  {
    return _positionsFlat[symbol];
  }

  for (auto& [id, pos] : _positionsOverflow)
  {
    if (id == symbol)
    {
      return pos;
    }
  }
  _positionsOverflow.emplace_back(symbol, Position{});
  return _positionsOverflow.back().second;
}

Volume BacktestResult::computePnl(Price entryPrice, Price exitPrice, Quantity qty, bool isLong)
{
  if (isLong)
  {
    const Price diff = Price::fromRaw(exitPrice.raw() - entryPrice.raw());
    return diff * qty;
  }
  else
  {
    const Price diff = Price::fromRaw(entryPrice.raw() - exitPrice.raw());
    return diff * qty;
  }
}

void BacktestResult::updatePositionLong(Position& pos, Quantity qty, Price price,
                                        UnixNanos timestampNs)
{
  if (pos.quantity.raw() >= 0)
  {
    const bool wasFlat = (pos.quantity.raw() == 0);
    const Volume existingValue = pos.avgPrice * pos.quantity;
    const Volume addedValue = price * qty;
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() + qty.raw());
    if (pos.quantity.raw() != 0)
    {
      const Volume totalValue = Volume::fromRaw(existingValue.raw() + addedValue.raw());
      pos.avgPrice = totalValue / pos.quantity;
    }
    if (wasFlat)
    {
      pos.entryTimeNs = timestampNs;
    }
  }
  else
  {
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() + qty.raw());
    if (pos.quantity.raw() > 0)
    {
      pos.avgPrice = price;
      pos.entryTimeNs = timestampNs;
    }
    else if (pos.quantity.raw() == 0)
    {
      pos.entryTimeNs = UnixNanos{};
    }
  }
}

void BacktestResult::updatePositionShort(Position& pos, Quantity qty, Price price,
                                         UnixNanos timestampNs)
{
  if (pos.quantity.raw() <= 0)
  {
    const bool wasFlat = (pos.quantity.raw() == 0);
    const Quantity absPos = Quantity::fromRaw(-pos.quantity.raw());
    const Volume existingValue = pos.avgPrice * absPos;
    const Volume addedValue = price * qty;
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() - qty.raw());
    if (pos.quantity.raw() != 0)
    {
      const Quantity newAbsPos = Quantity::fromRaw(-pos.quantity.raw());
      const Volume totalValue = Volume::fromRaw(existingValue.raw() + addedValue.raw());
      pos.avgPrice = totalValue / newAbsPos;
    }
    if (wasFlat)
    {
      pos.entryTimeNs = timestampNs;
    }
  }
  else
  {
    pos.quantity = Quantity::fromRaw(pos.quantity.raw() - qty.raw());
    if (pos.quantity.raw() < 0)
    {
      pos.avgPrice = price;
      pos.entryTimeNs = timestampNs;
    }
    else if (pos.quantity.raw() == 0)
    {
      pos.entryTimeNs = UnixNanos{};
    }
  }
}

void BacktestResult::recordTrade(SymbolId symbol, Side side, Price entryPrice, Price exitPrice,
                                 Quantity quantity, UnixNanos entryTimeNs, UnixNanos exitTimeNs,
                                 Volume pnl, Volume fee)
{
  TradeRecord trade;
  trade.symbol = symbol;
  trade.side = side;
  trade.entryPrice = entryPrice;
  trade.exitPrice = exitPrice;
  trade.quantity = quantity;
  trade.entryTimeNs = entryTimeNs;
  trade.exitTimeNs = exitTimeNs;
  trade.pnl = pnl;
  trade.fee = fee;
  _trades.push_back(trade);

  const int64_t netPnlRaw = pnl.raw() - fee.raw();
  _totalPnl = Volume::fromRaw(_totalPnl.raw() + pnl.raw());
  _currentEquity = Volume::fromRaw(_currentEquity.raw() + netPnlRaw);

  if (_currentEquity.raw() > _peakEquity.raw())
  {
    _peakEquity = _currentEquity;
  }

  const int64_t drawdownRaw = _peakEquity.raw() - _currentEquity.raw();
  if (drawdownRaw > _maxDrawdown.raw())
  {
    _maxDrawdown = Volume::fromRaw(drawdownRaw);
  }

  EquityPoint pt;
  pt.timestampNs = exitTimeNs;
  pt.equity = _currentEquity.toDouble();
  const double peak = _peakEquity.toDouble();
  pt.drawdownPct = (peak > 0.0) ? (drawdownRaw / static_cast<double>(_peakEquity.raw())) * 100.0 : 0.0;
  if (pt.drawdownPct > _maxDrawdownPct)
  {
    _maxDrawdownPct = pt.drawdownPct;
  }
  _equityCurve.push_back(pt);
}

void BacktestResult::setFeeSchedule(const FeeSchedule& schedule)
{
  _fees = schedule;
  // A schedule bound to an account reads its 30-day notional from that
  // account and writes fills back into it. Unbind the copy and carry the
  // number over by value, so replaying the run neither depends on nor
  // disturbs the venue's live counter. An unbound schedule already owns its
  // rolling window, which the copy brought with it -- there is nothing to
  // carry over in that case.
  _feeBaseNotional30d =
      (schedule.boundAccount() != nullptr) ? schedule.rollingNotional30d() : 0.0;
  _feeBaseApplied = false;
  _fees->clearAccountBinding();
}

Volume BacktestResult::computeFee(Price price, Quantity qty, bool isMaker, UnixNanos tsNs)
{
  if (!_config.usePercentageFee)
  {
    return Volume::fromDouble(_config.fixedFeePerTrade);
  }

  const Volume notional = price * qty;

  if (_fees.has_value() && _fees->tierCount() > 0)
  {
    if (!_feeBaseApplied)
    {
      // First fill of the replay: now there is a timestamp to stamp the
      // pre-run volume with, so it sits inside the 30-day window rather than
      // at the epoch, where the first eviction would drop it.
      _feeBaseApplied = true;
      if (_feeBaseNotional30d > 0.0)
      {
        _fees->recordFill(tsNs.raw(), _feeBaseNotional30d);
      }
    }
    const double fee = _fees->feeFor(tsNs.raw(), notional.toDouble(), isMaker);
    _fees->recordFill(tsNs.raw(), notional.toDouble());
    return Volume::fromDouble(fee);
  }

  const std::optional<double>& sideRate =
      isMaker ? _config.makerFeeRate : _config.takerFeeRate;
  // An untouched config leaves both sides empty and keeps charging feeRate,
  // so results do not move. A rate that is set is used as given, sign and
  // all: a negative one is a rebate the venue pays.
  const double rate = sideRate.value_or(_config.feeRate);
  return Volume::fromRaw(static_cast<int64_t>(notional.toDouble() * rate *
                                              static_cast<double>(Volume::Scale)));
}

namespace
{
// Builds the per-period return series from the equity curve. Each return is
// relative to the previous equity point (or to initialCapital for the first
// point). Risk-free rate is subtracted so returns are already "excess".
std::vector<double> buildReturnSeries(const std::vector<EquityPoint>& curve,
                                      double initialCapital, double riskFreeRate)
{
  std::vector<double> r;
  if (curve.empty() || initialCapital <= 0.0)
  {
    return r;
  }
  r.reserve(curve.size());
  double prev = initialCapital;
  for (const auto& pt : curve)
  {
    if (prev <= 0.0)
    {
      break;
    }
    r.push_back((pt.equity - prev) / prev - riskFreeRate);
    prev = pt.equity;
  }
  return r;
}
}  // namespace

double BacktestResult::computeSharpeRatio() const
{
  const auto returns = buildReturnSeries(_equityCurve, _config.initialCapital,
                                         _config.riskFreeRate);
  if (returns.size() < 2)
  {
    return 0.0;
  }

  double sum = 0.0;
  double sumSq = 0.0;
  for (double r : returns)
  {
    sum += r;
    sumSq += r * r;
  }
  const double n = static_cast<double>(returns.size());
  const double mean = sum / n;
  const double variance = (sumSq / n) - mean * mean;
  if (variance <= 0.0)
  {
    return 0.0;
  }
  const double stddev = std::sqrt(variance);
  const double annualization = std::sqrt(_config.metricsAnnualizationFactor);
  return (mean / stddev) * annualization;
}

double BacktestResult::computeSortinoRatio() const
{
  const auto returns = buildReturnSeries(_equityCurve, _config.initialCapital,
                                         _config.riskFreeRate);
  if (returns.size() < 2)
  {
    return 0.0;
  }

  double sum = 0.0;
  double downsideSumSq = 0.0;
  for (double r : returns)
  {
    sum += r;
    if (r < 0.0)
    {
      downsideSumSq += r * r;
    }
  }
  const double n = static_cast<double>(returns.size());
  const double mean = sum / n;
  const double downsideStddev = std::sqrt(downsideSumSq / n);
  if (downsideStddev <= 0.0)
  {
    return 0.0;
  }
  const double annualization = std::sqrt(_config.metricsAnnualizationFactor);
  return (mean / downsideStddev) * annualization;
}

double BacktestResult::computeCalmarRatio(double twr) const
{
  if (_trades.empty() || _maxDrawdown.raw() <= 0 || _config.initialCapital <= 0.0)
  {
    return 0.0;
  }

  const double maxDDPct = _maxDrawdownPct / 100.0;
  if (maxDDPct <= 0.0)
  {
    return 0.0;
  }

  // Annualize the cumulative TWR using the trade-based sampling rate.
  // periods per year = metricsAnnualizationFactor (e.g. 252 for daily sampling).
  // If we observed n periods, annualized return = (1 + twr)^(periodsPerYear/n) - 1.
  //
  // With very few sampled periods this exponent explodes: two points against
  // a 252-periods-per-year factor raises (1 + twr) to the 126th power, which
  // produced Calmar values on the order of 1e124 on short equity curves.
  // Refuse to annualize below a minimum sample count instead of extrapolating
  // a two-point curve into a bogus "per year" figure.
  static constexpr double kMinCalmarPeriods = 5.0;
  const double periods = static_cast<double>(_equityCurve.size());
  if (periods < kMinCalmarPeriods)
  {
    return 0.0;
  }
  const double ratio = _config.metricsAnnualizationFactor / periods;
  const double annualizedReturn = std::pow(1.0 + twr, ratio) - 1.0;
  return annualizedReturn / maxDDPct;
}

double BacktestResult::computeTimeWeightedReturn() const
{
  // TWR chains per-period returns computed from consecutive equity points.
  // Returns the cumulative product minus 1 (i.e. total period return).
  if (_equityCurve.empty() || _config.initialCapital <= 0.0)
  {
    return 0.0;
  }

  double prev = _config.initialCapital;
  double product = 1.0;
  for (const auto& pt : _equityCurve)
  {
    if (prev <= 0.0)
    {
      break;
    }
    const double r = (pt.equity - prev) / prev;
    product *= (1.0 + r);
    prev = pt.equity;
  }
  return product - 1.0;
}

}  // namespace lrvx
