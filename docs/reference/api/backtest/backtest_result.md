# BacktestResult

`BacktestResult` collects fills during backtest execution and computes performance statistics, including an equity curve and a range of risk/reward metrics.

## BacktestConfig

```cpp
struct BacktestConfig
{
  double initialCapital{100000.0};
  double feeRate{0.0001};            // 0.01% per trade
  bool usePercentageFee{true};
  double fixedFeePerTrade{0.0};

  SlippageProfile defaultSlippage{};
  std::vector<std::pair<SymbolId, SlippageProfile>> perSymbolSlippage{};

  QueueModel queueModel{QueueModel::NONE};
  size_t queueDepth{8};
  double queuePositionMinChangeFraction{0.05};

  // Cancellation ack model
  int64_t cancelAckLatencyNs{0};
  int64_t cancelAckJitterNs{0};
  uint64_t cancelAckSeed{42};

  // Replace ack model
  int64_t replaceAckLatencyNs{0};
  int64_t replaceAckJitterNs{0};

  // Submit ack model
  int64_t submitAckLatencyNs{0};
  int64_t submitAckJitterNs{0};

  double riskFreeRate{0.0};
  double metricsAnnualizationFactor{252.0};
};
```

| Field | Default | Description |
|-------|---------|-------------|
| `initialCapital` | 100000.0 | Starting capital |
| `feeRate` | 0.0001 | Fee as fraction of notional (0.01%) |
| `usePercentageFee` | true | Use percentage fee vs fixed |
| `fixedFeePerTrade` | 0.0 | Fixed fee per trade when percentage is off |
| `defaultSlippage` | `{NONE}` | Slippage profile applied when no per-symbol override exists. See `slippage.md`. |
| `perSymbolSlippage` | `{}` | Per-symbol overrides of the default profile |
| `queueModel` | `NONE` | Queue simulation mode for limit orders. See `queue_simulation.md`. |
| `queueDepth` | 8 | Levels tracked in `FULL` mode |
| `queuePositionMinChangeFraction` | 0.05 | Minimum fractional change in `queueAhead` before a `QUEUE_POSITION_UPDATED` event fires. `0.0` fires on every change (lossless, very chatty on liquid books); `1.0` disables emission. Computed against the order's `aheadAtArrival`, so one threshold covers all sizes |
| `cancelAckLatencyNs` | 0 | Base round-trip delay between `cancelOrder()` and `CANCELED`. Zero preserves the synchronous legacy behavior. Positive turns cancels async: `PENDING_CANCEL` fires immediately, `CANCELED` when sim time reaches `now + sampled_latency`, and the order may still fill in that window (late-cancel-after-fill race) |
| `cancelAckJitterNs` | 0 | Uniform jitter band; sampled latency is drawn from `[base - jitter, base + jitter]` |
| `cancelAckSeed` | 42 | Seed making the ack-latency sampling reproducible across runs. Shared by the replace model |
| `replaceAckLatencyNs` | 0 | Mirrors the cancel model. Positive turns replace async: `REPLACE_SUBMITTED` fires immediately, `REPLACE_ACCEPTED` + `REPLACED` when the deadline elapses, and `REPLACE_REJECTED` when the original filled inside the window |
| `replaceAckJitterNs` | 0 | Jitter band for the replace ack |
| `submitAckLatencyNs` | 0 | Positive defers `ACCEPTED` until the sampled deadline. The order does not participate in queue matching or fills until then. A `POST_ONLY` order that becomes marketable during the window is `REJECTED` with `reject_reason = "late_post_only_crossed"` |
| `submitAckJitterNs` | 0 | Jitter band for the submit ack |
| `riskFreeRate` | 0.0 | Per-period risk-free rate subtracted from each trade return before Sharpe/Sortino |
| `metricsAnnualizationFactor` | 252.0 | Scaling factor used for annualized Sharpe/Sortino/Calmar (sqrt applied) |

## BacktestStats

```cpp
struct BacktestStats
{
  size_t totalTrades;
  size_t winningTrades;
  size_t losingTrades;

  double initialCapital;
  double finalCapital;
  double totalPnl;
  double totalFees;
  double netPnl;
  double grossProfit;
  double grossLoss;

  double maxDrawdown;
  double maxDrawdownPct;

  double winRate;
  double profitFactor;
  double avgWin;
  double avgLoss;
  double avgWinLossRatio;

  size_t maxConsecutiveWins;
  size_t maxConsecutiveLosses;

  double avgTradeDurationNs;
  double medianTradeDurationNs;
  double maxTradeDurationNs;

  double sharpeRatio;
  double sortinoRatio;
  double calmarRatio;
  double timeWeightedReturn;
  double returnPct;

  UnixNanos startTimeNs;
  UnixNanos endTimeNs;
};
```

### New metrics

| Metric | Description |
|--------|-------------|
| `avgWinLossRatio` | `avgWin / avgLoss` (undefined when no losses) |
| `maxConsecutiveWins` | Longest streak of profitable trades |
| `maxConsecutiveLosses` | Longest streak of losing trades |
| `avgTradeDurationNs` | Mean time from entry to exit in nanoseconds |
| `medianTradeDurationNs` | Median trade duration in nanoseconds |
| `maxTradeDurationNs` | Longest trade duration in nanoseconds |
| `timeWeightedReturn` | Cumulative product of per-trade returns minus 1 |

### Formula changes

Sharpe, Sortino and Calmar are computed from the equity curve. A per-period return is the relative change from the previous equity point (or from `initialCapital` for the first point), with `riskFreeRate` subtracted:

```
r_i = (equity[i] - equity[i-1]) / equity[i-1] - riskFreeRate
```

Sharpe and Sortino annualize the series with `sqrt(metricsAnnualizationFactor)`. Calmar is `annualizedReturn / maxDrawdownPct`, where `annualizedReturn = (1 + TWR)^(metricsAnnualizationFactor / n) - 1` and `n` is the number of observed periods (closed trades). Raising a short curve's return to a large power blows up fast: two periods against the default 252-per-year factor is already a 126th power. Calmar is `0.0` below a minimum of 5 observed periods, rather than annualizing a pathologically short curve into an astronomical number.

`TradeRecord.fee` now includes both the entry-fill fee and the exit-fill fee pro-rated by the quantity closed in the trade. Partial closes keep the residual entry fee accrued to the remaining open portion of the position.

`maxDrawdownPct` is measured against the peak equity that stood at the moment of the drawdown, the same denominator `EquityPoint.drawdownPct` uses point by point. The summary card and the drawdown timeline on a report page therefore agree. Dividing the absolute maximum by the peak as it stands at the end of the run instead would understate risk by however much the account grew afterwards: a curve running 1,000,000 to 700,000 to 3,000,000 has a 30% drawdown, and that denominator reports 10%. Calmar uses the same figure.

The per-trade fee split is carried at full width. The per-side fee in quote currency times the quantity passes the int64 ceiling at about 922, which a 7 BTC round trip at four basis points already does, and the narrow form wrapped there into a negative fee: equity after a flat round trip finished above where it started while `totalFees` and `netPnl` in the same object stayed correct.

### A position open after the last fill

`trades()` and `equityCurve()` hold closed trades only; `totalPnl` is their
realized PnL. `totalFees` is the fee of every recorded fill, including the
entry fill (or the part of it not yet closed) of a position still open after
the last fill. `netPnl = totalPnl - totalFees`, `finalCapital = initialCapital
+ netPnl` and `returnPct` therefore include that entry fee, and nothing values
the open position, so its unrealized PnL appears nowhere:

```
netPnl = sum over trades() of (pnl - fee) - entry fee of the open position
```

The last `EquityPoint.equity` is `initialCapital` plus the closed trades' net,
without that fee. For 0.01 BTC entered at 78,000 at 4 bp the two differ by
0.312. When no trade has closed, `computeStats()` returns zero for every
statistic, `totalFees` and `netPnl` included, and `finalCapital` equals
`initialCapital`.

There is no flag for an open position. `fills()` shows it: per symbol, the sum
of `quantity` with `+` for `BUY` and `-` for `SELL` is non-zero while a
position is open. A strategy driven by `BacktestRunner` can read its own
position at the end of the run (`ctx.position`, or `position()` in the Python
`on_stop()`).

## EquityPoint

```cpp
struct EquityPoint
{
  UnixNanos timestampNs;
  double equity;
  double drawdownPct;
};
```

One point is appended to the equity curve on every closed trade, and none for a position still open. The curve is accessible via `BacktestResult::equityCurve()` and can be written to a CSV file with header `timestamp_ns,equity,drawdown_pct` via `writeEquityCurveCsv(path)`.

## BacktestResult

```cpp
class BacktestResult
{
public:
  explicit BacktestResult(const BacktestConfig& config = {}, size_t expectedFills = 0);

  void recordFill(const Fill& fill);
  BacktestStats computeStats() const;

  const std::vector<Fill>& fills() const;
  const std::vector<TradeRecord>& trades() const;
  const std::vector<EquityPoint>& equityCurve() const;
  double totalPnl() const;

  bool writeEquityCurveCsv(const std::string& path) const;
};
```

Each `TradeRecord` now carries both the entry and exit price, the closed quantity, and the entry/exit timestamps. These fields drive the duration statistics and enable equity-curve analysis per trade.

## Usage

```cpp
BacktestConfig cfg;
// SlippageProfile is {model, ticks, tickSize, bps, impactCoeff} — five members,
// with Price tickSize third. Price has no implicit double conversion.
cfg.defaultSlippage = {SlippageModel::FIXED_BPS, 0, Price{}, 2.0, 0.0};  // 2 bps
cfg.queueModel = QueueModel::TOB;
cfg.riskFreeRate = 0.0;

BacktestResult result(cfg);
// feed fills via result.recordFill(...) or by running a BacktestRunner
auto stats = result.computeStats();

std::cout << "Return: " << stats.returnPct << "%\n";
std::cout << "Sharpe: " << stats.sharpeRatio << "\n";
std::cout << "Sortino: " << stats.sortinoRatio << "\n";
std::cout << "Calmar: " << stats.calmarRatio << "\n";
std::cout << "Max consecutive wins: " << stats.maxConsecutiveWins << "\n";
std::cout << "Avg duration (s): " << stats.avgTradeDurationNs / 1e9 << "\n";

result.writeEquityCurveCsv("equity.csv");
```
