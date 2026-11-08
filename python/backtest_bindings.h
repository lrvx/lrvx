// python/backtest_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_result.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/backtest/venue_availability.h"
#include "lrvx/common.h"
#include "order_type_bindings.h"

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace
{

using namespace lrvx;

#pragma pack(push, 1)
struct PyFill
{
  uint64_t order_id;
  uint32_t symbol;
  uint8_t side;
  uint8_t is_maker;
  uint8_t _pad[2];
  int64_t price_raw;
  int64_t quantity_raw;
  int64_t timestamp_ns;
};
#pragma pack(pop)
static_assert(sizeof(PyFill) == 40);

#pragma pack(push, 1)
struct PyTradeRecord
{
  uint32_t symbol;
  uint8_t side;
  uint8_t _pad[3];
  int64_t entry_price_raw;
  int64_t exit_price_raw;
  int64_t quantity_raw;
  int64_t entry_time_ns;
  int64_t exit_time_ns;
  int64_t pnl_raw;
  int64_t fee_raw;
};
#pragma pack(pop)
static_assert(sizeof(PyTradeRecord) == 64);

#pragma pack(push, 1)
struct PyEquityPoint
{
  int64_t timestamp_ns;
  double equity;
  double drawdown_pct;
};
#pragma pack(pop)
static_assert(sizeof(PyEquityPoint) == 24);

inline PyFill fillToPyFill(const Fill& f)
{
  return {.order_id = f.orderId,
          .symbol = f.symbol,
          .side = static_cast<uint8_t>(f.side == Side::BUY ? 0 : 1),
          .is_maker = static_cast<uint8_t>(f.isMaker ? 1 : 0),
          ._pad = {},
          .price_raw = f.price.raw(),
          .quantity_raw = f.quantity.raw(),
          .timestamp_ns = f.timestampNs.raw()};
}

inline PyTradeRecord tradeRecToPy(const TradeRecord& t)
{
  return {.symbol = t.symbol,
          .side = static_cast<uint8_t>(t.side == Side::BUY ? 0 : 1),
          ._pad = {},
          .entry_price_raw = t.entryPrice.raw(),
          .exit_price_raw = t.exitPrice.raw(),
          .quantity_raw = t.quantity.raw(),
          .entry_time_ns = t.entryTimeNs.raw(),
          .exit_time_ns = t.exitTimeNs.raw(),
          .pnl_raw = t.pnl.raw(),
          .fee_raw = t.fee.raw()};
}

inline PyEquityPoint equityToPy(const EquityPoint& p)
{
  return {.timestamp_ns = p.timestampNs.raw(),
          .equity = p.equity,
          .drawdown_pct = p.drawdownPct};
}

inline SlippageProfile makePyProfile(const std::string& model, int32_t ticks,
                                     double tick_size, double bps, double impact)
{
  SlippageProfile p;
  if (model == "fixed_ticks")
  {
    p.model = SlippageModel::FIXED_TICKS;
  }
  else if (model == "fixed_bps")
  {
    p.model = SlippageModel::FIXED_BPS;
  }
  else if (model == "volume_impact")
  {
    p.model = SlippageModel::VOLUME_IMPACT;
  }
  else
  {
    p.model = SlippageModel::NONE;
  }
  p.ticks = ticks;
  p.tickSize = (tick_size > 0.0) ? Price::fromDouble(tick_size) : Price{};
  p.bps = bps;
  p.impactCoeff = impact;
  return p;
}

// Wraps SimulatedExecutor + SimulatedClock for standalone use
class PySimulatedExecutor
{
 public:
  PySimulatedExecutor() : _executor(_clock) { _executor.start(); }

  // For other bindings that need to wire the same executor into
  // their own subsystem (e.g. LiquidationEngine::setExecutor).
  SimulatedExecutor& executor() { return _executor; }

  void submitOrder(uint64_t id, const std::string& sideStr, double price, double qty,
                   const std::string& typeStr, uint32_t symbol,
                   const std::string& tifStr = "gtc", bool reduceOnly = false,
                   int64_t expiresAtNs = 0, uint64_t accountId = 0,
                   double trigger = 0.0, double trailingOffset = 0.0,
                   int32_t trailingBps = 0)
  {
    Order order;
    order.id = id;
    order.side = (sideStr == "buy") ? Side::BUY : Side::SELL;
    order.price = Price::fromDouble(price);
    order.quantity = Quantity::fromDouble(qty);
    order.symbol = symbol;
    order.type = parseOrderType(typeStr);
    order.timeInForce = parseTimeInForce(tifStr);
    order.flags.reduceOnly = reduceOnly ? 1 : 0;
    order.accountId = accountId;
    if (expiresAtNs > 0)
    {
      order.expiresAfter = TimePoint(std::chrono::nanoseconds(expiresAtNs));
    }

    // Conditional orders keep the trigger separate from the limit
    // price; the simulator's trigger checks read Order::triggerPrice.
    // For the market-style conditionals a single price is unambiguous,
    // so `price` doubles as the trigger when the caller left `trigger`
    // unset. STOP_LIMIT / TAKE_PROFIT_LIMIT need both values and fall
    // back the same way only when no explicit trigger is given.
    const bool isConditional =
        order.type == OrderType::STOP_MARKET || order.type == OrderType::STOP_LIMIT ||
        order.type == OrderType::TAKE_PROFIT_MARKET ||
        order.type == OrderType::TAKE_PROFIT_LIMIT;
    if (isConditional)
    {
      order.triggerPrice = Price::fromDouble(trigger > 0.0 ? trigger : price);
    }
    if (order.type == OrderType::TRAILING_STOP)
    {
      order.trailingOffset = Price::fromDouble(trailingOffset);
      order.trailingCallbackRate = trailingBps;
    }

    _executor.submitOrder(order);
  }

  void setSTPGroupMembership(uint64_t account_id, uint64_t group_id)
  {
    _executor.setSTPGroupMembership(account_id, group_id);
  }
  uint64_t stpGroupFor(uint64_t account_id) const
  {
    return _executor.stpGroupFor(account_id);
  }

  static TimeInForce parseTimeInForce(const std::string& s)
  {
    if (s == "ioc")
    {
      return TimeInForce::IOC;
    }
    if (s == "fok")
    {
      return TimeInForce::FOK;
    }
    if (s == "gtd")
    {
      return TimeInForce::GTD;
    }
    if (s == "post_only")
    {
      return TimeInForce::POST_ONLY;
    }
    return TimeInForce::GTC;
  }

  void cancelOrder(uint64_t id) { _executor.cancelOrder(id); }
  void cancelAll(uint32_t symbol) { _executor.cancelAllOrders(symbol); }

  void submitBracket(uint64_t bracketId, uint32_t symbol,
                     const std::string& entrySide, const std::string& entryType,
                     double entryPrice, double quantity,
                     const std::string& tpSide, const std::string& tpType, double tpPrice,
                     const std::string& stopSide, const std::string& stopType,
                     double stopTriggerPrice)
  {
    BracketOrder b;
    b.bracketId = bracketId;
    b.symbol = symbol;
    auto parseSide = [](const std::string& s)
    { return s == "buy" ? Side::BUY : Side::SELL; };
    b.entry.side = parseSide(entrySide);
    b.entry.type = parseOrderType(entryType);
    b.entry.price = Price::fromDouble(entryPrice);
    b.entry.quantity = Quantity::fromDouble(quantity);
    b.takeProfit.side = parseSide(tpSide);
    b.takeProfit.type = parseOrderType(tpType);
    b.takeProfit.price = Price::fromDouble(tpPrice);
    b.takeProfit.quantity = Quantity::fromDouble(quantity);
    b.stop.side = parseSide(stopSide);
    b.stop.type = parseOrderType(stopType);
    b.stop.triggerPrice = Price::fromDouble(stopTriggerPrice);
    b.stop.quantity = Quantity::fromDouble(quantity);
    _executor.submitBracket(b);
  }
  void cancelBracket(uint64_t bracketId) { _executor.cancelBracket(bracketId); }
  void setBracketChildArmMode(const std::string& mode)
  {
    _executor.setBracketChildArmMode(
        mode == "on_partial_fill"
            ? SimulatedExecutor::BracketArmMode::OnPartialFill
            : SimulatedExecutor::BracketArmMode::OnFullFill);
  }
  std::string bracketState(uint64_t bracketId) const
  {
    auto st = _executor.bracketStatus(bracketId).state;
    switch (st)
    {
      case BracketState::PENDING_ENTRY:
        return "pending_entry";
      case BracketState::ENTRY_FILLED:
        return "entry_filled";
      case BracketState::TP_FILLED:
        return "tp_filled";
      case BracketState::STOP_FILLED:
        return "stop_filled";
      case BracketState::CANCELED:
        return "canceled";
    }
    return "pending_entry";
  }

  void submitIceberg(uint64_t id, const std::string& sideStr, double price,
                     double totalQty, double visibleQty, uint32_t symbol)
  {
    Order o;
    o.id = id;
    o.side = (sideStr == "buy") ? Side::BUY : Side::SELL;
    o.price = Price::fromDouble(price);
    o.quantity = Quantity::fromDouble(totalQty);
    o.visibleQuantity = Quantity::fromDouble(visibleQty);
    o.type = OrderType::ICEBERG;
    o.symbol = symbol;
    _executor.submitOrder(o);
  }

  void setIcebergRefreshLatency(int64_t latencyNs)
  {
    _executor.setIcebergRefreshLatency(latencyNs);
  }
  void setIcebergSizeRandomisationPct(double pct)
  {
    _executor.setIcebergSizeRandomisationPct(pct);
  }
  void setIcebergPriorityMode(const std::string& mode)
  {
    _executor.setIcebergPriorityModeByName(mode);
  }
  void setIcebergJitterSeed(uint64_t seed)
  {
    _executor.setIcebergJitterSeed(seed);
  }

  int64_t icebergHiddenRemainingRaw(uint64_t id) const
  {
    return _executor.icebergHiddenRemainingRaw(id);
  }

  void onBar(uint32_t symbol, double closePrice)
  {
    _executor.onBar(symbol, Price::fromDouble(closePrice));
  }

  // Open-aware form: moves the market to the open (releasing any order held
  // from the previous bar's callback there), then walks low -> high -> close.
  // Needed for a caller driving SimulatedExecutor by hand to reach the same
  // fills as BacktestRunner.run_bars, which uses this overload internally.
  void onBarOhlc(uint32_t symbol, double openPrice, double highPrice, double lowPrice,
                 double closePrice)
  {
    _executor.onBar(symbol, Price::fromDouble(openPrice), Price::fromDouble(highPrice),
                    Price::fromDouble(lowPrice), Price::fromDouble(closePrice));
  }

  void beginBarCallbackWindow() { _executor.beginBarCallbackWindow(); }
  void endBarCallbackWindow() { _executor.endBarCallbackWindow(); }

  // Drops fills, live/held/conditional orders and run-scoped state while
  // keeping installed configuration (slippage, queue model, latency, ...).
  // Lets a caller replay several hand-driven runs through one executor.
  void reset() { _executor.reset(); }

  void onTrade(uint32_t symbol, double price, bool isBuy)
  {
    _executor.onTrade(symbol, Price::fromDouble(price), isBuy);
  }

  void onTradeQty(uint32_t symbol, double price, double qty, bool isBuy)
  {
    _executor.onTrade(symbol, Price::fromDouble(price), Quantity::fromDouble(qty), isBuy);
  }

  // Routed to the touch path for the same reason as the C API's
  // on_best_levels: a one-level book here would replace the ladder.
  void onBestLevels(uint32_t symbol, double bidPrice, double bidQty, double askPrice,
                    double askQty)
  {
    onTopOfBook(symbol, std::make_pair(bidPrice, bidQty), std::make_pair(askPrice, askQty));
  }

  void onTopOfBook(uint32_t symbol, const std::optional<std::pair<double, double>>& bid,
                   const std::optional<std::pair<double, double>>& ask)
  {
    std::pmr::monotonic_buffer_resource pool(256);
    std::pmr::vector<BookLevel> bids(&pool);
    std::pmr::vector<BookLevel> asks(&pool);
    if (bid)
    {
      bids.emplace_back(Price::fromDouble(bid->first), Quantity::fromDouble(bid->second));
    }
    if (ask)
    {
      asks.emplace_back(Price::fromDouble(ask->first), Quantity::fromDouble(ask->second));
    }
    _executor.onTopOfBook(symbol, bids, asks);
  }

  void onBookSnapshot(uint32_t symbol,
                      const std::vector<std::pair<double, double>>& bidLevels,
                      const std::vector<std::pair<double, double>>& askLevels)
  {
    std::pmr::monotonic_buffer_resource pool(1024);
    std::pmr::vector<BookLevel> bids(&pool);
    std::pmr::vector<BookLevel> asks(&pool);
    bids.reserve(bidLevels.size());
    asks.reserve(askLevels.size());
    for (const auto& [price, qty] : bidLevels)
    {
      bids.emplace_back(Price::fromDouble(price), Quantity::fromDouble(qty));
    }
    for (const auto& [price, qty] : askLevels)
    {
      asks.emplace_back(Price::fromDouble(price), Quantity::fromDouble(qty));
    }
    _executor.onBookUpdate(symbol, bids, asks);
  }

  void advanceClock(int64_t timestampNs) { _clock.advanceTo(UnixNanos::fromRaw(timestampNs)); }

  void setDefaultSlippage(const std::string& model, int32_t ticks, double tick_size,
                          double bps, double impact)
  {
    _executor.setDefaultSlippage(makePyProfile(model, ticks, tick_size, bps, impact));
  }

  void setSymbolSlippage(uint32_t symbol, const std::string& model, int32_t ticks,
                         double tick_size, double bps, double impact)
  {
    _executor.setSymbolSlippage(symbol,
                                makePyProfile(model, ticks, tick_size, bps, impact));
  }

  void setQueueModel(const std::string& model, uint32_t depth)
  {
    QueueModel qm = QueueModel::NONE;
    if (model == "tob")
    {
      qm = QueueModel::TOB;
    }
    else if (model == "full")
    {
      qm = QueueModel::FULL;
    }
    else if (model == "pro_rata")
    {
      qm = QueueModel::PRO_RATA;
    }
    else if (model == "pro_rata_with_fifo")
    {
      qm = QueueModel::PRO_RATA_WITH_FIFO;
    }
    else if (model == "top_pro_lmm")
    {
      qm = QueueModel::TOP_PRO_LMM;
    }
    else if (model == "pro_rata_with_priority")
    {
      qm = QueueModel::PRO_RATA_WITH_PRIORITY;
    }
    _executor.setQueueModel(qm, depth);
  }

  void setQueueFifoTopN(uint32_t topN)
  {
    _executor.setQueueFifoTopN(topN);
  }

  void setTopPriorityShare(double share) { _executor.setTopPriorityShare(share); }
  void setLmmOrders(const std::vector<lrvx::OrderId>& ids) { _executor.setLmmOrders(ids); }
  void setLmmBonusMultiplier(double m) { _executor.setLmmBonusMultiplier(m); }
  void setOrderPriorityMultiplier(lrvx::OrderId id, double m)
  {
    _executor.setOrderPriorityMultiplier(id, m);
  }

  void setQueuePositionMinChangeFraction(double fraction)
  {
    _executor.setQueuePositionMinChangeFraction(fraction);
  }

  void setSubmitAckLatency(int64_t latencyNs, int64_t jitterNs)
  {
    _executor.setSubmitAckLatency(latencyNs, jitterNs);
  }
  void setCancelAckLatency(int64_t latencyNs, int64_t jitterNs)
  {
    _executor.setCancelAckLatency(latencyNs, jitterNs);
  }
  void setReplaceAckLatency(int64_t latencyNs, int64_t jitterNs)
  {
    _executor.setReplaceAckLatency(latencyNs, jitterNs);
  }
  void applyLatencyProfile(const std::string& name)
  {
    // A typo was a silent no-op: the run kept default latencies and looked
    // like a healthy venue-realistic backtest.
    if (!_executor.applyLatencyProfile(name.c_str()))
    {
      throw std::invalid_argument(
          "unknown latency profile: " + name +
          ". known: binance_um_futures, bybit_linear, okx_swap, deribit, "
          "idealized, adversarial");
    }
  }
  void setSTPMode(const std::string& mode)
  {
    lrvx::STPMode m = lrvx::STPMode::None;
    if (mode == "cancel_newest")
    {
      m = lrvx::STPMode::CancelNewest;
    }
    else if (mode == "cancel_oldest")
    {
      m = lrvx::STPMode::CancelOldest;
    }
    else if (mode == "cancel_both")
    {
      m = lrvx::STPMode::CancelBoth;
    }
    else if (mode == "decrement")
    {
      m = lrvx::STPMode::Decrement;
    }
    _executor.setSTPMode(m);
  }
  void setFokMode(const std::string& mode)
  {
    _executor.setFokModeByName(mode);
  }
  std::string fokMode() const
  {
    return (_executor.fokMode() == lrvx::SimulatedExecutor::FokMode::SinglePrice)
               ? "single_price"
               : "any_price";
  }
  void setSubmitAckLatencyDistribution(const lrvx::LatencyDistribution& d)
  {
    _executor.setSubmitAckLatencyDistribution(d);
  }
  void setCancelAckLatencyDistribution(const lrvx::LatencyDistribution& d)
  {
    _executor.setCancelAckLatencyDistribution(d);
  }
  void setReplaceAckLatencyDistribution(const lrvx::LatencyDistribution& d)
  {
    _executor.setReplaceAckLatencyDistribution(d);
  }
  void setRateLimitPolicy(const lrvx::RateLimitPolicy& p)
  {
    _executor.setRateLimitPolicy(p);
  }
  void clearRateLimitPolicy() { _executor.clearRateLimitPolicy(); }

  void setVenueAvailability(lrvx::VenueAvailability* va)
  {
    _executor.setVenueAvailability(va);
  }

  py::array_t<PyFill> fills() const
  {
    const auto& f = _executor.fills();
    py::array_t<PyFill> result(f.size());
    auto* out = result.mutable_data();
    for (size_t i = 0; i < f.size(); ++i)
    {
      out[i] = fillToPyFill(f[i]);
    }
    return result;
  }

  py::list fillsList() const
  {
    py::list result;
    for (const auto& f : _executor.fills())
    {
      py::dict d;
      d["order_id"] = f.orderId;
      d["symbol"] = f.symbol;
      d["side"] = (f.side == Side::BUY) ? "buy" : "sell";
      d["price"] = f.price.toDouble();
      d["quantity"] = f.quantity.toDouble();
      d["timestamp_ns"] = f.timestampNs.raw();
      d["is_maker"] = f.isMaker;
      result.append(d);
    }
    return result;
  }

  size_t fillCount() const { return _executor.fills().size(); }

  const std::vector<Fill>& rawFills() const { return _executor.fills(); }

 private:
  // The canonical table, not a copy of it: see
  // python/order_type_bindings.h. An unrecognised name raises instead of
  // quietly becoming a market order.
  static OrderType parseOrderType(const std::string& s)
  {
    return lrvx::parseOrderTypeStrict(s);
  }

  SimulatedClock _clock;
  SimulatedExecutor _executor;
};

class PyBacktestResult
{
 public:
  PyBacktestResult(double initialCapital, double feeRate, bool usePercentageFee,
                   double fixedFeePerTrade, double riskFreeRate, double annualization,
                   std::optional<double> makerFeeRate,
                   std::optional<double> takerFeeRate)
  {
    _config.initialCapital = initialCapital;
    _config.feeRate = feeRate;
    _config.usePercentageFee = usePercentageFee;
    _config.fixedFeePerTrade = fixedFeePerTrade;
    // None means unset, so both sides keep charging fee_rate. A negative rate
    // is a rebate the venue pays, not a missing value.
    _config.makerFeeRate = makerFeeRate;
    _config.takerFeeRate = takerFeeRate;
    _config.riskFreeRate = riskFreeRate;
    _config.metricsAnnualizationFactor = (annualization > 0.0) ? annualization : 252.0;
    _result = std::make_unique<BacktestResult>(_config);
  }

  void recordFill(uint64_t orderId, uint32_t symbol, const std::string& sideStr, double price,
                  double qty, int64_t timestampNs, bool isMaker)
  {
    Fill fill{};
    fill.orderId = orderId;
    fill.symbol = symbol;
    fill.side = (sideStr == "buy") ? Side::BUY : Side::SELL;
    fill.price = Price::fromDouble(price);
    fill.quantity = Quantity::fromDouble(qty);
    fill.timestampNs = UnixNanos::fromRaw(timestampNs);
    fill.isMaker = isMaker;
    _result->recordFill(fill);
  }

  void ingestExecutor(const PySimulatedExecutor& exec)
  {
    for (const auto& fill : exec.rawFills())
    {
      _result->recordFill(fill);
    }
  }

  py::dict stats() const
  {
    auto s = _result->computeStats();
    py::dict d;
    d["total_trades"] = s.totalTrades;
    d["winning_trades"] = s.winningTrades;
    d["losing_trades"] = s.losingTrades;
    d["max_consecutive_wins"] = s.maxConsecutiveWins;
    d["max_consecutive_losses"] = s.maxConsecutiveLosses;
    d["initial_capital"] = s.initialCapital;
    d["final_capital"] = s.finalCapital;
    d["total_pnl"] = s.totalPnl;
    d["total_fees"] = s.totalFees;
    d["net_pnl"] = s.netPnl;
    d["gross_profit"] = s.grossProfit;
    d["gross_loss"] = s.grossLoss;
    d["max_drawdown"] = s.maxDrawdown;
    d["max_drawdown_pct"] = s.maxDrawdownPct;
    d["win_rate"] = s.winRate;
    d["profit_factor"] = s.profitFactor;
    d["avg_win"] = s.avgWin;
    d["avg_loss"] = s.avgLoss;
    d["avg_win_loss_ratio"] = s.avgWinLossRatio;
    d["avg_trade_duration_ns"] = s.avgTradeDurationNs;
    d["median_trade_duration_ns"] = s.medianTradeDurationNs;
    d["max_trade_duration_ns"] = s.maxTradeDurationNs;
    d["sharpe_ratio"] = s.sharpeRatio;
    d["sortino_ratio"] = s.sortinoRatio;
    d["calmar_ratio"] = s.calmarRatio;
    d["time_weighted_return"] = s.timeWeightedReturn;
    d["return_pct"] = s.returnPct;
    d["start_time_ns"] = s.startTimeNs.raw();
    d["end_time_ns"] = s.endTimeNs.raw();
    return d;
  }

  py::array_t<PyTradeRecord> trades() const
  {
    const auto& t = _result->trades();
    py::array_t<PyTradeRecord> arr(t.size());
    auto* out = arr.mutable_data();
    for (size_t i = 0; i < t.size(); ++i)
    {
      out[i] = tradeRecToPy(t[i]);
    }
    return arr;
  }

  py::array_t<PyEquityPoint> equityCurve() const
  {
    const auto& c = _result->equityCurve();
    py::array_t<PyEquityPoint> arr(c.size());
    auto* out = arr.mutable_data();
    for (size_t i = 0; i < c.size(); ++i)
    {
      out[i] = equityToPy(c[i]);
    }
    return arr;
  }

  bool writeEquityCurveCsv(const std::string& path) const
  {
    return _result->writeEquityCurveCsv(path);
  }

 private:
  BacktestConfig _config;
  std::unique_ptr<BacktestResult> _result;
};

}  // namespace

inline void bindBacktest(py::module_& m)
{
  py::class_<lrvx::LatencyDistribution>(m, "LatencyDistribution")
      .def(py::init<>())
      .def_static("constant", &lrvx::LatencyDistribution::constant, py::arg("ns"))
      .def_static("uniform", &lrvx::LatencyDistribution::uniform,
                  py::arg("lo_ns"), py::arg("hi_ns"))
      .def_static("lognormal", &lrvx::LatencyDistribution::lognormal,
                  py::arg("median_ns"), py::arg("sigma"))
      .def_static("empirical", &lrvx::LatencyDistribution::empirical,
                  py::arg("samples_ns"))
      .def("set_burst_correlation",
           &lrvx::LatencyDistribution::setBurstCorrelation, py::arg("rho"))
      .def_property("burst_correlation",
                    &lrvx::LatencyDistribution::burstCorrelation,
                    &lrvx::LatencyDistribution::setBurstCorrelation)
      .def("median_ns", &lrvx::LatencyDistribution::medianNs);

  PYBIND11_NUMPY_DTYPE(PyFill, order_id, symbol, side, is_maker, price_raw, quantity_raw,
                       timestamp_ns);
  PYBIND11_NUMPY_DTYPE(PyTradeRecord, symbol, side, entry_price_raw, exit_price_raw,
                       quantity_raw, entry_time_ns, exit_time_ns, pnl_raw, fee_raw);
  PYBIND11_NUMPY_DTYPE(PyEquityPoint, timestamp_ns, equity, drawdown_pct);

  py::class_<PySimulatedExecutor>(m, "SimulatedExecutor")
      .def(py::init<>())
      .def("submit_order", &PySimulatedExecutor::submitOrder,
           "Submit an order to the simulated exchange. type: one of "
           "lrvx.ORDER_TYPE_NAMES (market|limit|stop_market|stop_limit|"
           "tp_market|tp_limit|trailing_stop|iceberg); anything else raises "
           "ValueError. tif: gtc|ioc|fok|gtd|post_only. "
           "reduce_only: only reduce existing position. expires_at_ns: GTD deadline. "
           "account_id: optional STP account identifier (default 0). "
           "trigger: trigger price for stop_market / stop_limit / "
           "tp_market / tp_limit (falls back to `price` "
           "when unset, which is unambiguous for the market-style "
           "conditionals). trailing_offset / trailing_bps: fixed-price or "
           "bps offset for trailing_stop (set exactly one).",
           py::arg("id"), py::arg("side"), py::arg("price"), py::arg("quantity"),
           py::arg("type") = "market", py::arg("symbol") = 1,
           py::arg("tif") = "gtc", py::arg("reduce_only") = false,
           py::arg("expires_at_ns") = 0, py::arg("account_id") = 0,
           py::arg("trigger") = 0.0, py::arg("trailing_offset") = 0.0,
           py::arg("trailing_bps") = 0)
      .def("set_stp_group_membership", &PySimulatedExecutor::setSTPGroupMembership,
           "Opt an STP account into a group. group_id=0 removes the mapping.",
           py::arg("account_id"), py::arg("group_id"))
      .def("stp_group_for", &PySimulatedExecutor::stpGroupFor,
           py::arg("account_id"))
      .def("cancel_order", &PySimulatedExecutor::cancelOrder, py::arg("order_id"))
      .def("cancel_all", &PySimulatedExecutor::cancelAll, py::arg("symbol"))
      .def("submit_bracket", &PySimulatedExecutor::submitBracket,
           "Submit a native bracket order (entry + take-profit + stop). "
           "The simulator manages the state machine: first child to fill "
           "cancels the other; cancelling cancels every live leg.",
           py::arg("bracket_id"), py::arg("symbol"), py::arg("entry_side"),
           py::arg("entry_type"), py::arg("entry_price"), py::arg("quantity"),
           py::arg("tp_side"), py::arg("tp_type"), py::arg("tp_price"),
           py::arg("stop_side"), py::arg("stop_type"), py::arg("stop_trigger_price"))
      .def("cancel_bracket", &PySimulatedExecutor::cancelBracket,
           py::arg("bracket_id"))
      .def("set_bracket_child_arm_mode",
           &PySimulatedExecutor::setBracketChildArmMode,
           "Child-arm policy: 'on_full_fill' (default) arms TP+stop once on "
           "full entry fill; 'on_partial_fill' arms / resizes children "
           "incrementally on every partial.",
           py::arg("mode"))
      .def("bracket_state", &PySimulatedExecutor::bracketState,
           "Bracket state: pending_entry | entry_filled | tp_filled | stop_filled | canceled.",
           py::arg("bracket_id"))
      .def("submit_iceberg", &PySimulatedExecutor::submitIceberg,
           "Submit a native iceberg order. Only visible_quantity is exposed; "
           "the hidden remainder refreshes automatically as the visible slice fills.",
           py::arg("order_id"), py::arg("side"), py::arg("price"),
           py::arg("total_quantity"), py::arg("visible_quantity"),
           py::arg("symbol") = 1)
      .def("set_iceberg_refresh_latency", &PySimulatedExecutor::setIcebergRefreshLatency,
           "Default refresh latency (ns) between a visible tranche filling "
           "and the next one being exposed. 0 = instant (most venues).",
           py::arg("latency_ns"))
      .def("iceberg_hidden_remaining_raw",
           &PySimulatedExecutor::icebergHiddenRemainingRaw,
           "Remaining hidden quantity (raw fixed-point) for an iceberg order.",
           py::arg("order_id"))
      .def("set_iceberg_size_randomisation_pct",
           &PySimulatedExecutor::setIcebergSizeRandomisationPct,
           "Per-refresh size jitter as a fraction (0.0 deterministic, "
           "0.10 = ±10% uniform).",
           py::arg("pct"))
      .def("set_iceberg_priority_mode",
           &PySimulatedExecutor::setIcebergPriorityMode,
           "Queue priority on refresh: 'back' (default) or 'retain' (CME-style).",
           py::arg("mode"))
      .def("set_iceberg_jitter_seed",
           &PySimulatedExecutor::setIcebergJitterSeed,
           "Seed the size-jitter RNG for reproducible refresh sequences.",
           py::arg("seed"))
      .def("on_bar", &PySimulatedExecutor::onBar,
           "Feed a bar close price for order matching",
           py::arg("symbol"), py::arg("close_price"))
      .def("on_bar_ohlc", &PySimulatedExecutor::onBarOhlc,
           "Feed a full OHLC bar: moves the market to the open (releasing any "
           "order held from the previous bar's callback there), then walks "
           "low -> high -> close so resting stops/targets match the intrabar "
           "extremes. Use this, not on_bar, when driving the executor by hand "
           "to reach the fills BacktestRunner.run_bars produces.",
           py::arg("symbol"), py::arg("open"), py::arg("high"), py::arg("low"),
           py::arg("close"))
      .def("begin_bar_callback_window", &PySimulatedExecutor::beginBarCallbackWindow,
           "Open the bar-callback window: every order submitted while it is "
           "open is held instead of matched immediately, and released at the "
           "next bar's open. Call before invoking a strategy's bar callback "
           "by hand; always pair with end_bar_callback_window.")
      .def("end_bar_callback_window", &PySimulatedExecutor::endBarCallbackWindow,
           "Close the bar-callback window opened by begin_bar_callback_window.")
      .def("reset", &PySimulatedExecutor::reset,
           "Drop fills and run-scoped state while keeping installed "
           "configuration (slippage, queue model, latency, ...), so a second "
           "hand-driven run reports that run and not the sum of every run.")
      .def("on_trade", &PySimulatedExecutor::onTrade,
           "Feed a trade for order matching",
           py::arg("symbol"), py::arg("price"), py::arg("is_buy"))
      .def("on_trade_qty", &PySimulatedExecutor::onTradeQty,
           "Feed a trade with quantity (enables queue-fill simulation)",
           py::arg("symbol"), py::arg("price"), py::arg("quantity"), py::arg("is_buy"))
      .def("on_best_levels", &PySimulatedExecutor::onBestLevels,
           "Feed both best levels in one call. Routed to the touch path: depth from the "
           "last on_book_snapshot survives instead of being replaced by a one-level book",
           py::arg("symbol"), py::arg("bid_price"), py::arg("bid_qty"),
           py::arg("ask_price"), py::arg("ask_qty"))
      .def("on_book_snapshot", &PySimulatedExecutor::onBookSnapshot,
           "Feed a full L2 snapshot. bid_levels and ask_levels are lists of (price, qty) tuples",
           py::arg("symbol"), py::arg("bid_levels"), py::arg("ask_levels"))
      .def("on_top_of_book", &PySimulatedExecutor::onTopOfBook,
           "Feed a touch-only frame: moves the touch, keeps deeper depth from the last "
           "snapshot. bid / ask are (price, qty) tuples; None means that side of the "
           "book is empty",
           py::arg("symbol"), py::arg("bid"), py::arg("ask"))
      .def("advance_clock", &PySimulatedExecutor::advanceClock,
           "Advance simulation clock to timestamp",
           py::arg("timestamp_ns"))
      .def("set_default_slippage", &PySimulatedExecutor::setDefaultSlippage,
           "Configure default slippage. model: none|fixed_ticks|fixed_bps|volume_impact."
           " tick_size is in price units (0.0 falls back to one raw price unit).",
           py::arg("model"), py::arg("ticks") = 0, py::arg("tick_size") = 0.0,
           py::arg("bps") = 0.0, py::arg("impact_coeff") = 0.0)
      .def("set_symbol_slippage", &PySimulatedExecutor::setSymbolSlippage,
           "Configure slippage for a specific symbol",
           py::arg("symbol"), py::arg("model"), py::arg("ticks") = 0,
           py::arg("tick_size") = 0.0, py::arg("bps") = 0.0,
           py::arg("impact_coeff") = 0.0)
      .def("set_queue_model", &PySimulatedExecutor::setQueueModel,
           "Configure queue simulation. model: none|tob|full|pro_rata|pro_rata_with_fifo|"
           "top_pro_lmm|pro_rata_with_priority",
           py::arg("model"), py::arg("depth") = 1)
      .def("set_queue_fifo_top_n", &PySimulatedExecutor::setQueueFifoTopN,
           "For pro_rata_with_fifo: first N orders at a level consume the trade "
           "FIFO; the rest is split pro-rata.",
           py::arg("top_n"))
      .def("set_top_priority_share", &PySimulatedExecutor::setTopPriorityShare,
           "TOP_PRO_LMM: fraction of each trade reserved for the queue-front order.",
           py::arg("share"))
      .def("set_lmm_orders", &PySimulatedExecutor::setLmmOrders,
           "TOP_PRO_LMM: mark these order ids as Lead Market Makers.",
           py::arg("ids"))
      .def("set_lmm_bonus_multiplier", &PySimulatedExecutor::setLmmBonusMultiplier,
           "TOP_PRO_LMM: LMM bonus multiplier on tail pro-rata distribution.",
           py::arg("multiplier"))
      .def("set_order_priority_multiplier",
           &PySimulatedExecutor::setOrderPriorityMultiplier,
           "TOP_PRO_LMM / PRO_RATA_WITH_PRIORITY: per-order priority weight (default 1.0).",
           py::arg("order_id"), py::arg("multiplier"))
      .def("set_queue_position_min_change_fraction",
           &PySimulatedExecutor::setQueuePositionMinChangeFraction,
           "Minimum fractional change in queue-ahead required to emit a "
           "queue-position event. 0.0 = every change, 1.0 = disable.",
           py::arg("fraction"))
      .def("set_submit_ack_latency",
           &PySimulatedExecutor::setSubmitAckLatency,
           py::arg("latency_ns"), py::arg("jitter_ns") = 0)
      .def("set_cancel_ack_latency",
           &PySimulatedExecutor::setCancelAckLatency,
           py::arg("latency_ns"), py::arg("jitter_ns") = 0)
      .def("set_replace_ack_latency",
           &PySimulatedExecutor::setReplaceAckLatency,
           py::arg("latency_ns"), py::arg("jitter_ns") = 0)
      .def("set_submit_ack_latency_distribution",
           &PySimulatedExecutor::setSubmitAckLatencyDistribution, py::arg("dist"))
      .def("set_cancel_ack_latency_distribution",
           &PySimulatedExecutor::setCancelAckLatencyDistribution, py::arg("dist"))
      .def("set_replace_ack_latency_distribution",
           &PySimulatedExecutor::setReplaceAckLatencyDistribution, py::arg("dist"))
      .def("apply_latency_profile",
           &PySimulatedExecutor::applyLatencyProfile,
           "Apply a named latency profile: binance_um_futures, "
           "bybit_linear, okx_swap, deribit, idealized, adversarial.",
           py::arg("name"))
      .def("fills", &PySimulatedExecutor::fills,
           "Get all fills as numpy structured array")
      .def("fills_list", &PySimulatedExecutor::fillsList,
           "Get all fills as list of dicts")
      .def("set_rate_limit_policy", &PySimulatedExecutor::setRateLimitPolicy,
           py::arg("policy"))
      .def("set_stp_mode", &PySimulatedExecutor::setSTPMode,
           "Self-trade prevention mode: none | cancel_newest | cancel_oldest | "
           "cancel_both | decrement.",
           py::arg("mode"))
      .def("set_fok_mode", &PySimulatedExecutor::setFokMode,
           "FOK fill semantic: any_price (default) or single_price.",
           py::arg("mode"))
      .def("fok_mode", &PySimulatedExecutor::fokMode)
      .def("clear_rate_limit_policy", &PySimulatedExecutor::clearRateLimitPolicy)
      .def("set_venue_availability", &PySimulatedExecutor::setVenueAvailability,
           "Attach a VenueAvailability model. None to detach.",
           py::arg("availability"), py::keep_alive<1, 2>())
      .def_property_readonly("fill_count", &PySimulatedExecutor::fillCount);

  py::class_<PyBacktestResult>(m, "BacktestResult")
      .def(py::init<double, double, bool, double, double, double,
                    std::optional<double>, std::optional<double>>(),
           py::arg("initial_capital") = 100000.0, py::arg("fee_rate") = 0.0001,
           py::arg("use_percentage_fee") = true, py::arg("fixed_fee_per_trade") = 0.0,
           py::arg("risk_free_rate") = 0.0, py::arg("annualization_factor") = 252.0,
           py::arg("maker_fee_rate") = py::none(),
           py::arg("taker_fee_rate") = py::none(),
           "maker_fee_rate / taker_fee_rate default to None, meaning unset: both\n"
           "sides are charged fee_rate. Set either to charge the real spread\n"
           "between posting and taking. A negative rate is a rebate the venue\n"
           "pays, not a missing value. Only used when use_percentage_fee.")
      .def("record_fill", &PyBacktestResult::recordFill,
           py::arg("order_id"), py::arg("symbol"), py::arg("side"), py::arg("price"),
           py::arg("quantity"), py::arg("timestamp_ns"), py::arg("is_maker") = false)
      .def("ingest_executor", &PyBacktestResult::ingestExecutor, py::arg("executor"),
           "Drain executor fills into this result in FIFO order")
      .def("stats", &PyBacktestResult::stats,
           "Compute stats as a dict (includes new metrics: streaks, durations, TWR)")
      .def("trades", &PyBacktestResult::trades,
           "Return trade records as numpy structured array")
      .def("equity_curve", &PyBacktestResult::equityCurve,
           "Return equity curve as numpy structured array")
      .def("write_equity_curve_csv", &PyBacktestResult::writeEquityCurveCsv, py::arg("path"),
           "Write equity curve to CSV (timestamp_ns,equity,drawdown_pct header)");
}
