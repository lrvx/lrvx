"""
lrvx -- Python bindings
"""
from __future__ import annotations
import collections.abc
import numpy
import numpy.typing
import typing
from . import _heatmap as _heatmap
from . import targets
__all__: list[str] = ['_heatmap', 'ALL_OR_NOTHING', 'ATR', 'Account', 'AdlRanking', 'AggregatorEventFilter', 'AmmCurve', 'AutoCorrelation', 'BEST_EFFORT', 'BacktestResult', 'BacktestRunner', 'BarData', 'BarDispatchRecorder', 'BinCountAggregator', 'Binance', 'BinaryLogRecorderHook', 'Bollinger', 'BookAnchored', 'BookOnly', 'BookSnapshotBinAggregator', 'Bybit', 'CANCELLED', 'CAPI_ABI_VERSION', 'CCI', 'CompositeBookMatrix', 'ConstantLatency', 'Correlation', 'Cross', 'DEMA', 'DataReader', 'DataWriter', 'DeltaBookEncoder', 'DeltaBookReplayer', 'EMA', 'EmpiricalLatency', 'Engine', 'EventTypeStatsAggregator', 'ExchangeCapabilities', 'ExecutionListener', 'Executor', 'ExponentialLatency', 'FAILED', 'FILLED', 'FIRE_ON_ANY', 'FeeSchedule', 'FeeTier', 'FeedClockPolicy', 'FillLiquidity', 'FootprintBar', 'FundingPayment', 'FundingSchedule', 'FundingTapeEntry', 'GaussianLatency', 'GridSearch', 'IndicatorGraph', 'Isolated', 'KAMA', 'KillSwitch', 'Kurtosis', 'L3Book', 'LEADER_FOLLOWER', 'LatencyDistribution', 'LatencyModel', 'LatencySample', 'LegState', 'LiquidationEngine', 'LiveQueuePositionEstimator', 'LrvxError', 'MACD', 'MarginMode', 'MarkImpactModel', 'MarketDataRecorderHook', 'MarketProfile', 'MergedTapeReader', 'MultiFeedClock', 'None_', 'OHLCBinAggregator', 'ONE_SIDED', 'ORDER_FLAG_IOC', 'ORDER_FLAG_POST_ONLY', 'ORDER_FLAG_REDUCE_ONLY', 'ORDER_TYPE_NAMES', 'OptionType', 'Order', 'OrderBook', 'OrderEventData', 'OrderEventKind', 'OrderGroup', 'OrderGroupPolicy', 'OrderGroupState', 'OrderJourneyTracer', 'OrderTracker', 'OrderValidator', 'PARTIALLY_FILLED', 'PENDING', 'PRICE_SCALE', 'ParkinsonVol', 'Partitioner', 'PeakAggregator', 'PnLTracker', 'PnlRatio', 'PoolReplay', 'PoolTape', 'PositionGroupTracker', 'PositionSize', 'PositionTracker', 'QUANTITY_SCALE', 'QUEUE_FULL', 'QUEUE_NONE', 'QUEUE_TOB', 'QuantileAggregator', 'REVERTING', 'RMA', 'RSI', 'RateLimitEndpointFamily', 'RateLimitPolicy', 'ReplayEvent', 'ReplaySource', 'RiskManager', 'RogersSatchellVol', 'RollingZScore', 'Runner', 'SIGNAL_FLAG_ENTER', 'SIGNAL_FLAG_EXIT', 'SIGNAL_FLAG_REBALANCE', 'SLIPPAGE_FIXED_BPS', 'SLIPPAGE_FIXED_TICKS', 'SLIPPAGE_NONE', 'SLIPPAGE_VOLUME_IMPACT', 'SMA', 'SUBMITTED', 'ShannonEntropy', 'Signal', 'SignalBuilder', 'SimulatedClock', 'SimulatedExecutor', 'Skewness', 'Slope', 'Stats', 'Stochastic', 'StorageSink', 'Strategy', 'StreamingIndicatorGraph', 'Symbol', 'SymbolContext', 'SymbolRegistry', 'TEMA', 'TapeRef', 'TraceReader', 'TraceRecorder', 'TradeData', 'VOLUME_SCALE', 'VenueAvailability', 'VenueExecutor', 'VenueStack', 'VolSurface', 'VolumeBinAggregator', 'VolumeProfile', 'WAIT_FOR_ALL', 'WalkForwardRunner', 'adf', 'adx', 'aggregate_heikin_ashi_bars', 'aggregate_range_bars', 'aggregate_renko_bars', 'aggregate_tick_bars', 'aggregate_time_bars', 'aggregate_volume_bars', 'atr', 'autocorrelation', 'bar_returns', 'baw_price', 'binomial_price', 'bollinger', 'bootstrap_ci', 'bs_price', 'bs_price_discrete_dividends', 'bs_vega', 'build_surface_as_of', 'calibrate_svi', 'capi_abi_version', 'cci', 'chop', 'correlation', 'cost_of_carry', 'cvd', 'deflated_sharpe', 'dema', 'ema', 'export_data', 'extract_symbols', 'extract_time_range', 'forward_price', 'greeks', 'grid_neighborhood', 'hurst_dfa', 'i256_roundtrip', 'implied_percentile_in_cone', 'implied_vol', 'inspect', 'kama', 'kurtosis', 'list_indicators', 'macd', 'merge', 'merge_dir', 'obv', 'order_type_code', 'order_type_name', 'parkinson_vol', 'pbo_cscv', 'permutation_test', 'prices_to_double', 'profit_factor', 'quantities_to_double', 'realized_vol', 'recompress', 'rma', 'rogers_satchell_vol', 'rolling_correlation', 'rolling_hurst', 'rolling_zscore', 'rsi', 'second_order_greeks', 'set_log_callback', 'shannon_entropy', 'skewness', 'slope', 'sma', 'split', 'stochastic', 'targets', 'tema', 'trade_pnl', 'u256_from_hex', 'u256_roundtrip', 'validate', 'validate_dataset', 'vol_cone', 'volumes_to_double', 'vwap', 'whites_reality_check', 'win_rate']
class ATR:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Account:
    def __init__(self, account_id: typing.SupportsInt | typing.SupportsIndex = 0, equity: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        ...
    def account_id(self) -> int:
        ...
    def add_equity(self, delta: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def close_position(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def cross_headroom(self, tier_fraction: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def equity(self) -> float:
        ...
    def has_stale_marks(self, now_ns: typing.SupportsInt | typing.SupportsIndex, budget_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    def margin_mode(self) -> MarginMode:
        ...
    def margin_notional(self) -> float:
        ...
    def margin_unrealised_pnl(self) -> float:
        ...
    def mark_for(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        ...
    def mark_ts_for(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def open_position(self, symbol: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, entry_price: typing.SupportsFloat | typing.SupportsIndex, isolated_equity: typing.SupportsFloat | typing.SupportsIndex = 0.0, contract_multiplier: typing.SupportsFloat | typing.SupportsIndex = 1.0, is_long_option: bool = False) -> None:
        ...
    def position_count(self) -> int:
        ...
    def record_fill(self, ts_ns: typing.SupportsInt | typing.SupportsIndex, notional: typing.SupportsFloat | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def reset(self, equity: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Clear every open position and mark, reset the 30-day rolling notional window, and set equity to `equity`. Use this to reuse one Account across repeated backtest or RL-training episodes on the same tape -- without it, a position and its equity delta from one episode survive into the next.
        """
    def reset_rolling(self) -> None:
        ...
    def rolling_notional_30d(self) -> float:
        ...
    def rolling_notional_by_symbol_30d(self) -> list[tuple[int, float]]:
        ...
    def set_equity(self, equity: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def set_margin_mode(self, mode: typing.Any) -> None:
        """
        Accepts a MarginMode enum or a string name (cross, isolated).
        """
    def set_mark(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def total_notional(self) -> float:
        ...
    def total_unrealised_pnl(self) -> float:
        ...
class AdlRanking:
    """
    Members:
    
      PnlRatio
    
      Binance
    
      Bybit
    
      PositionSize
    """
    Binance: typing.ClassVar[AdlRanking]  # value = <AdlRanking.Binance: 1>
    Bybit: typing.ClassVar[AdlRanking]  # value = <AdlRanking.Bybit: 2>
    PnlRatio: typing.ClassVar[AdlRanking]  # value = <AdlRanking.PnlRatio: 0>
    PositionSize: typing.ClassVar[AdlRanking]  # value = <AdlRanking.PositionSize: 3>
    __members__: typing.ClassVar[dict[str, AdlRanking]]  # value = {'PnlRatio': <AdlRanking.PnlRatio: 0>, 'Binance': <AdlRanking.Binance: 1>, 'Bybit': <AdlRanking.Bybit: 2>, 'PositionSize': <AdlRanking.PositionSize: 3>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class AggregatorEventFilter:
    """
    Which event kinds an aggregator counts. Trades = TradeRecord only, BooksOnly = BookSnapshot + BookDelta only, Both = every event.
    
    Members:
    
      Trades
    
      BooksOnly
    
      Both
    """
    BooksOnly: typing.ClassVar[AggregatorEventFilter]  # value = <AggregatorEventFilter.BooksOnly: 2>
    Both: typing.ClassVar[AggregatorEventFilter]  # value = <AggregatorEventFilter.Both: 3>
    Trades: typing.ClassVar[AggregatorEventFilter]  # value = <AggregatorEventFilter.Trades: 1>
    __members__: typing.ClassVar[dict[str, AggregatorEventFilter]]  # value = {'Trades': <AggregatorEventFilter.Trades: 1>, 'BooksOnly': <AggregatorEventFilter.BooksOnly: 2>, 'Both': <AggregatorEventFilter.Both: 3>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class AmmCurve:
    @staticmethod
    def constant_product(reserve0: int, reserve1: int, fee_num: typing.SupportsInt | typing.SupportsIndex = 997, fee_den: typing.SupportsInt | typing.SupportsIndex = 1000) -> AmmCurve:
        ...
    @staticmethod
    def raydium_cp(reserve0: int, reserve1: int, trade_fee_rate: typing.SupportsInt | typing.SupportsIndex, creator_fee_rate: typing.SupportsInt | typing.SupportsIndex = 0, creator_fee_on_input: bool = True) -> AmmCurve:
        ...
    @staticmethod
    def uniswap_v3(sqrt_price_x96: int, liquidity: int, fee_pips: typing.SupportsInt | typing.SupportsIndex, ticks: list = []) -> AmmCurve:
        ...
    def amount_out(self, i: typing.SupportsInt | typing.SupportsIndex, j: typing.SupportsInt | typing.SupportsIndex, amount_in: int) -> int:
        ...
    def apply_swap(self, i: typing.SupportsInt | typing.SupportsIndex, j: typing.SupportsInt | typing.SupportsIndex, amount_in: int) -> int:
        ...
    def balances(self) -> list:
        ...
    def clone(self) -> AmmCurve:
        ...
    def liquidity(self) -> int | None:
        ...
    def sqrt_price(self) -> int | None:
        ...
    def token_count(self) -> int:
        ...
class AutoCorrelation:
    def __init__(self, window: typing.SupportsInt | typing.SupportsIndex, lag: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class BacktestResult:
    def __init__(self, initial_capital: typing.SupportsFloat | typing.SupportsIndex = 100000.0, fee_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0001, use_percentage_fee: bool = True, fixed_fee_per_trade: typing.SupportsFloat | typing.SupportsIndex = 0.0, risk_free_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, annualization_factor: typing.SupportsFloat | typing.SupportsIndex = 252.0, maker_fee_rate: typing.SupportsFloat | typing.SupportsIndex | None = None, taker_fee_rate: typing.SupportsFloat | typing.SupportsIndex | None = None) -> None:
        """
        maker_fee_rate / taker_fee_rate default to None, meaning unset: both
        sides are charged fee_rate. Set either to charge the real spread
        between posting and taking. A negative rate is a rebate the venue
        pays, not a missing value. Only used when use_percentage_fee.
        """
    def equity_curve(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Return equity curve as numpy structured array
        """
    def ingest_executor(self, executor: SimulatedExecutor) -> None:
        """
        Drain executor fills into this result in FIFO order
        """
    def record_fill(self, order_id: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, timestamp_ns: typing.SupportsInt | typing.SupportsIndex, is_maker: bool = False) -> None:
        ...
    def stats(self) -> dict:
        """
        Compute stats as a dict (includes new metrics: streaks, durations, TWR)
        """
    def trades(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Return trade records as numpy structured array
        """
    def write_equity_curve_csv(self, path: str) -> bool:
        """
        Write equity curve to CSV (timestamp_ns,equity,drawdown_pct header)
        """
class BacktestRunner:
    def __init__(self, registry: SymbolRegistry, fee_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0004, initial_capital: typing.SupportsFloat | typing.SupportsIndex = 100000.0) -> None:
        ...
    def add_execution_listener(self, listener: ExecutionListener) -> None:
        ...
    def equity_curve(self) -> typing.Any:
        """
        Return the equity curve from the most recent run as a dict of numpy arrays (timestamp_ns, equity, drawdown_pct).
        """
    def run_bars(self, start_time_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], end_time_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], open: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], symbol: str = '', bar_type: typing.SupportsInt | typing.SupportsIndex = 0, bar_type_param: typing.SupportsInt | typing.SupportsIndex = 0) -> typing.Any:
        ...
    def run_csv(self, path: str, symbol: str = '') -> typing.Any:
        ...
    def run_ohlcv(self, ts: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], symbol: str = '') -> typing.Any:
        ...
    def run_tape(self, path: str) -> typing.Any:
        """
        Run a backtest off a `.lrvx` tape directory. The tape is the canonical recorded artifact (`lrvx tape record` writes it). Returns the same stats dict shape as run_csv / run_bars.
        """
    def run_tapes(self, paths: collections.abc.Sequence[str]) -> typing.Any:
        """
        Run a backtest off N `.lrvx` tapes merged on read. Symbols are rekeyed by (metadata.exchange, name) so two captures of the same venue/symbol collapse, and two different venues stay distinct. `run_tapes([t])` is equivalent to `run_tape(t)`. Stats shape mirrors run_tape.
        """
    @typing.overload
    def set_executor(self, executor: Executor) -> None:
        ...
    @typing.overload
    def set_executor(self, executor: VenueExecutor) -> None:
        """
        Refused: pass the stack via set_venue_stack(stack) instead. The executor alone does not carry the clock the runner must advance.
        """
    def set_kill_switch(self, ks: KillSwitch) -> None:
        """
        Attach (or detach with None) a kill switch. Reduce-only orders bypass so tightening caps does not strand a position.
        """
    def set_order_validator(self, ov: OrderValidator) -> None:
        """
        Attach (or detach with None) an order validator. Reduce-only orders bypass.
        """
    def set_pnl_tracker(self, tracker: PnLTracker) -> None:
        """
        Attach (or detach with None) a PnL tracker. Fires `on_signal(signal)` for every fill the simulator dispatches.
        """
    def set_risk_manager(self, rm: RiskManager) -> None:
        """
        Attach (or detach with None) a pre-trade risk manager. Reduce-only orders bypass the gate by design.
        """
    def set_strategy(self, strategy: Strategy) -> None:
        ...
    def set_venue_stack(self, stack: VenueStack) -> None:
        """
        Run against a VenueStack: the runner feeds the stack executor market data and harvests its fills, so the run uses the venue's fill mechanics (queue model, iceberg latency, venue availability, rate limits) and its fee ladder -- each fill is billed at the tier the stack's 30-day notional resolves to instead of the flat fee_rate. Funding and liquidation are still driven by explicit calls rather than the replay loop. Pass None to revert to the built-in executor.
        """
    def trades(self) -> typing.Any:
        """
        Return closed trades from the most recent run as a dict of numpy arrays (symbol, side, entry_price, exit_price, quantity, pnl, fee, entry_time_ns, exit_time_ns).
        """
class BarData:
    symbol_name: str
    def __init__(self) -> None:
        ...
    @property
    def bar_type(self) -> int:
        ...
    @bar_type.setter
    def bar_type(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def bar_type_param(self) -> int:
        ...
    @bar_type_param.setter
    def bar_type_param(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def buy_volume(self) -> float:
        ...
    @buy_volume.setter
    def buy_volume(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def close(self) -> float:
        ...
    @close.setter
    def close(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def close_reason(self) -> int:
        ...
    @close_reason.setter
    def close_reason(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def end_time_ns(self) -> int:
        ...
    @end_time_ns.setter
    def end_time_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def high(self) -> float:
        ...
    @high.setter
    def high(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def low(self) -> float:
        ...
    @low.setter
    def low(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def open(self) -> float:
        ...
    @open.setter
    def open(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def start_time_ns(self) -> int:
        ...
    @start_time_ns.setter
    def start_time_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def volume(self) -> float:
        ...
    @volume.setter
    def volume(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class BarDispatchRecorder:
    def __init__(self) -> None:
        ...
    def add_time_interval_seconds(self, seconds: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def count(self) -> int:
        ...
    def finalize(self) -> None:
        ...
    def on_trade(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def param_at(self, index: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def type_at(self, index: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
class BinCountAggregator(_AggregatorHandle):
    """
    Time-bucketed event count. Each event floors to its bucket (floor(exchange_ts_ns / bucket_ns) * bucket_ns) and increments the (bucket, optional symbol, optional side) cell. Result is a flat structured numpy array of (bucket, symbol_id, side, count) rows.
    """
    def __init__(self, bucket_ns: typing.SupportsInt | typing.SupportsIndex, by_side: bool = False, by_symbol: bool = False, event_filter: AggregatorEventFilter = ..., symbol_filter: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> None:
        ...
    def result(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Structured numpy: (bucket_ts_ns i8, symbol_id u4, side u1, count u8). side encoding: 0 = aggregate, 1 = BUY, 2 = SELL. symbol_id is 0 when by_symbol=False.
        """
class BinaryLogRecorderHook:
    def __init__(self, output_dir: str, max_segment_mb: typing.SupportsInt | typing.SupportsIndex = 256, exchange_id: typing.SupportsInt | typing.SupportsIndex = 0, compression: str = 'none', exchange_name: str = '', instrument_type: str = '') -> None:
        """
        Create a binary-log recorder hook. Trades and books are written via BinaryLogWriter on the engine thread — no Python callback per event. exchange_name + instrument_type are stamped into the recording's metadata.json so MergedTapeReader can key by (exchange, name) across multiple tapes.
        """
    def add_symbol(self, symbol_id: typing.SupportsInt | typing.SupportsIndex, name: str, base: str = '', quote: str = '', price_precision: typing.SupportsInt | typing.SupportsIndex = 8, qty_precision: typing.SupportsInt | typing.SupportsIndex = 8) -> None:
        """
        Register a symbol for recording metadata
        """
    def close(self) -> None:
        """
        Stop recording (idempotent; engine on_stop also calls this)
        """
    def current_segment_path(self) -> str:
        """
        Return the path of the segment currently being written
        """
    def flush(self) -> None:
        """
        Flush buffered data to disk
        """
    def stats(self) -> dict:
        """
        Return recorder statistics as a dict
        """
class Bollinger:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex = 20, multiplier: typing.SupportsFloat | typing.SupportsIndex = 2.0) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> dict:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def lower(self) -> float | None:
        ...
    @property
    def middle(self) -> float | None:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def upper(self) -> float | None:
        ...
    @property
    def value(self) -> float | None:
        ...
class BookSnapshotBinAggregator(_AggregatorHandle):
    """
    Time-bucketed order-book snapshots. Maintains the full ladder per symbol from book snapshot/delta events and at each bucket boundary emits the latest state observed inside the closed bucket as up to `levels` rows per side (row = bid+ask paired at the same depth, shorter side zero-padded). Buckets without book events produce no rows; the last observed state is emitted as the trailing bucket. Book reconstruction is order-dependent across the whole tape, so this aggregator REQUIRES DataReader.run(..., n_threads=1) — parallel runs raise.
    """
    def __init__(self, bucket_ns: typing.SupportsInt | typing.SupportsIndex, levels: typing.SupportsInt | typing.SupportsIndex = 20, event_filter: AggregatorEventFilter = ..., symbol_filter: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> None:
        ...
    def result(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Structured numpy: (bucket_ts_ns i8, symbol_id u4, level u2, flags u2, bid_price_raw i8, bid_qty_raw i8, ask_price_raw i8, ask_qty_raw i8). level 0 = top of book; zero price+qty on a side = no level at that depth. flags bit 0 = book crossed (best bid >= best ask) at this bucket. *_raw fields are fixed-point — divide by Price/Quantity SCALE. Sorted by (bucket_ts_ns, symbol_id, level).
        """
class CCI:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex = 20) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class CompositeBookMatrix:
    """
    Cross-exchange composite order book. Tracks best bid/ask across up to 4 exchanges per symbol.
    """
    def __init__(self, staleness_threshold_ms: typing.SupportsInt | typing.SupportsIndex = 5000) -> None:
        """
        Create a CompositeBookMatrix with staleness threshold in milliseconds
        """
    def ask_for_exchange(self, symbol: typing.SupportsInt | typing.SupportsIndex, exchange: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best ask on a specific exchange or None
        """
    def best_ask(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best ask across all exchanges (dict with price, quantity, exchange) or None
        """
    def best_bid(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best bid across all exchanges (dict with price, quantity, exchange) or None
        """
    def bid_for_exchange(self, symbol: typing.SupportsInt | typing.SupportsIndex, exchange: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best bid on a specific exchange or None
        """
    def check_staleness(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Check all entries against staleness threshold using current timestamp
        """
    def has_arbitrage_opportunity(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Check if best bid on one exchange > best ask on another
        """
    def mark_exchange_stale(self, exchange: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Mark all symbols on an exchange as stale
        """
    def mark_stale(self, exchange: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Mark a specific exchange+symbol as stale
        """
    def spread(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Composite spread (best ask - best bid) or None
        """
    def update_book(self, exchange: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, bid_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], bid_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], recv_ns: typing.SupportsInt | typing.SupportsIndex = 0, is_delta: bool = False) -> None:
        """
        Feed a book update from an exchange. is_delta=False (the default) replaces both sides of that exchange's top-of-book wholesale, as a real snapshot would. is_delta=True only updates the side(s) present in bid_prices/ask_prices; a side not present in a delta call is left exactly as it was, not cleared.
        """
class ConstantLatency(LatencyModel):
    """
    Returns the same nanoseconds every call. Useful as a baseline.
    """
    def __init__(self, feed_ns: typing.SupportsInt | typing.SupportsIndex = 0, order_ns: typing.SupportsInt | typing.SupportsIndex = 0, fill_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
class Correlation:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, x: typing.SupportsFloat | typing.SupportsIndex, y: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class DEMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class DataReader:
    def __init__(self, data_dir: str, from_ns: typing.Any = None, to_ns: typing.Any = None, symbols: typing.Any = None, reorder_window_ns: typing.Any = None, strict_ordering: bool = False) -> None:
        """
        Create a DataReader for a binary log data directory. `reorder_window_ns` controls the bounded reorder buffer applied to segments without the Sorted flag (default 10s). An event that arrives past that window is dropped and counted in stats()['late_dropped'], with one warning logged per segment; set `strict_ordering=True` to raise LrvxError E_DATA_002 on the first such event instead. `to_ns` is inclusive for trade reads and exclusive for option-quote reads -- see read_option_quotes.
        """
    def count(self) -> int:
        """
        Return the total number of events
        """
    def read_bbo(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read best bid/ask from every book update as a numpy structured array (PyBBO dtype)
        """
    def read_bbo_from(self, start_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read BBO starting from a given timestamp (nanoseconds)
        """
    def read_book_updates(self) -> tuple:
        """
        Read all book updates. Returns (headers, levels) tuple of numpy structured arrays. headers dtype: PyBookUpdateHeader; levels dtype: PyLevel. For event i: bids=levels[h['level_offset']:h['level_offset']+h['bid_count']], asks=levels[h['level_offset']+h['bid_count']:h['level_offset']+h['bid_count']+h['ask_count']]
        """
    def read_book_updates_from(self, start_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> tuple:
        """
        Read book updates starting from a given timestamp (nanoseconds). Same return shape as read_book_updates().
        """
    def read_option_quotes_from(self, start_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read option quotes (mark/iv/index/open-interest) from a given timestamp (nanoseconds) as a numpy structured array (PyOptionQuote dtype). Raw fixed-point: mark/index use PRICE_SCALE, iv uses 1e8, oi uses QUANTITY_SCALE. The reader's to_ns bound is EXCLUSIVE here, unlike read_trades: a quote stamped exactly to_ns belongs to the next window, so day-by-day slicing of a month returns each quote once.
        """
    def read_trades(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read all trades as a numpy structured array (PyTrade dtype). The reader's to_ns bound is INCLUSIVE here: a trade stamped exactly to_ns is returned.
        """
    def read_trades_from(self, start_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read trades starting from a given timestamp (nanoseconds). The reader's to_ns bound is inclusive.
        """
    def run(self, aggregators: list, n_threads: typing.SupportsInt | typing.SupportsIndex = 0, progress_callback: typing.Any = None, progress_interval_ms: typing.SupportsInt | typing.SupportsIndex = 1000) -> bool:
        """
        Run a panel of streaming aggregators over the tape in a single decompression pass. Each aggregator's onEvent fires once per event, then finalize() fires once at the end. An empty list is a no-op (no decompression). GIL released for the whole walk. n_threads=0 (default) is auto, resolved to min(blocks_per_segment/2, hardware_concurrency); n_threads=1 forces explicit single-thread; n_threads>1 partitions each segment at the compressed-block level via IAggregator::cloneEmpty()/merge() and is capped to the effective block count. progress_callback, when set, fires inside the run loop at most once per progress_interval_ms with (pct: float in [0,1], cursor_ts_ns: int). Return False to cancel; raising stops the run and re-raises after finalize. Progress is reported only on the single-thread path (n_threads=1); multi-thread runs ignore the callback because per-event GIL re-acquisition would defeat the parallelism.
        """
    def segment_files(self) -> list:
        """
        Return list of segment file paths
        """
    def segments(self) -> list:
        """
        Return list of segment info dicts
        """
    def stats(self) -> dict:
        """
        Return reader statistics as a dict
        """
    def summary(self) -> dict:
        """
        Return a dict summarizing the dataset
        """
    def symbols(self) -> set:
        """
        Return the set of available symbol IDs
        """
    def time_range(self) -> typing.Any:
        """
        Return (start_ns, end_ns) tuple or None
        """
class DataWriter:
    def __init__(self, output_dir: str, max_segment_mb: typing.SupportsInt | typing.SupportsIndex = 256, exchange_id: typing.SupportsInt | typing.SupportsIndex = 0, compression: str = 'none') -> None:
        """
        Create a DataWriter for binary log output
        """
    def close(self) -> None:
        """
        Close the writer and finalize all segments
        """
    def current_segment_path(self) -> str:
        """
        Return the path of the current segment being written
        """
    def flush(self) -> None:
        """
        Flush buffered data to disk
        """
    def stats(self) -> dict:
        """
        Return writer statistics as a dict
        """
    def write_book(self, exchange_ts_ns: typing.SupportsInt | typing.SupportsIndex, recv_ts_ns: typing.SupportsInt | typing.SupportsIndex, seq: typing.SupportsInt | typing.SupportsIndex, symbol_id: typing.SupportsInt | typing.SupportsIndex, is_snapshot: bool, bids: typing.Annotated[numpy.typing.ArrayLike, ...], asks: typing.Annotated[numpy.typing.ArrayLike, ...]) -> bool:
        """
        Write a single book update. bids/asks are PyLevel structured arrays.
        """
    def write_books(self, headers: typing.Annotated[numpy.typing.ArrayLike, ...], levels: typing.Annotated[numpy.typing.ArrayLike, ...]) -> int:
        """
        Batched book writer. Round-trip with DataReader.read_book_updates.
        """
    def write_option_quotes(self, exchange_ts_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], recv_ts_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], mark_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], index_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ivs: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], open_interest: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], symbol_ids: typing.Annotated[numpy.typing.ArrayLike, numpy.uint32], bid_prices: typing.Any = None, ask_prices: typing.Any = None, underlying_prices: typing.Any = None, bid_sizes: typing.Any = None, ask_sizes: typing.Any = None, bid_ivs: typing.Any = None, ask_ivs: typing.Any = None) -> int:
        """
        Bulk-write option quotes. mark_prices/index_prices are doubles (PRICE_SCALE), ivs are doubles (e.g. 0.65), open_interest is a double (QUANTITY_SCALE). The rest are optional (None -> 0): bid_prices/ask_prices and bid_sizes/ask_sizes give the touch the fill model crosses; underlying_prices is the per-expiry forward (pricing/moneyness) vs index_prices (spot index); bid_ivs/ask_ivs are the vol at each touch. Returns the count written.
        """
    def write_trade(self, exchange_ts_ns: typing.SupportsInt | typing.SupportsIndex, recv_ts_ns: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, trade_id: typing.SupportsInt | typing.SupportsIndex, symbol_id: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Write a single trade record
        """
    def write_trades(self, exchange_ts_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], recv_ts_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], trade_ids: typing.Annotated[numpy.typing.ArrayLike, numpy.uint64], symbol_ids: typing.Annotated[numpy.typing.ArrayLike, numpy.uint32], sides: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> int:
        """
        Write trades from numpy arrays (vectorized). Returns number of trades written.
        """
class DeltaBookEncoder:
    """
    Encode a stream of L2 snapshots as anchor snapshots plus deltas. anchor_every=0 emits snapshots only; the existing tape format stores either type, so callers can safely persist the output.
    """
    def __init__(self, anchor_every: typing.SupportsInt | typing.SupportsIndex = 100) -> None:
        ...
    def encode(self, symbol_id: typing.SupportsInt | typing.SupportsIndex, bids: list, asks: list) -> dict:
        """
        Feed the encoder a full snapshot. Returns a dict with is_delta / bids / asks. The bids and asks lists carry BookLevel-shaped dicts with price_raw + qty_raw.
        """
    def reset(self, symbol_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def reset_all(self) -> None:
        ...
class DeltaBookReplayer:
    """
    Reverse of DeltaBookEncoder. Maintains current book state per symbol; given a stream of (type, bids, asks) tuples returns the reconstructed full snapshot at each step.
    """
    def __init__(self) -> None:
        ...
    def anchored(self, symbol_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Whether this symbol has seen a snapshot and is tracking a complete book.
        """
    def apply(self, type: typing.SupportsInt | typing.SupportsIndex, symbol_id: typing.SupportsInt | typing.SupportsIndex, bids: list, asks: list) -> dict:
        """
        Apply one event. type=0 snapshot, type=1 delta. Returns the reconstructed full snapshot for the symbol, with 'anchored' False when no snapshot has been seen yet -- in which case the delta is refused and the level lists come back empty, because merging it into nothing would produce a book missing everything before the entry point.
        """
    def reset(self, symbol_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def reset_all(self) -> None:
        ...
class EMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class EmpiricalLatency(LatencyModel):
    """
    Resample with replacement from observed values. Pass three lists of measured latencies (one per component).
    """
    def __init__(self, feed_samples: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = [], order_samples: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = [], fill_samples: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = [], seed: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
class Engine:
    def __init__(self, initial_capital: typing.SupportsFloat | typing.SupportsIndex = 100000.0, fee_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0001) -> None:
        ...
    def bar_count(self, symbol: str = '') -> int:
        ...
    def close(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def high(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def load_csv(self, path: str, symbol: str = '') -> None:
        ...
    def load_df(self, df: typing.Any, symbol: str = '') -> None:
        ...
    def load_ohlcv(self, data: dict, symbol: str = '') -> None:
        ...
    def low(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def open(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def resample(self, symbol: str, target: str, interval: str) -> None:
        ...
    def run(self, signals: SignalBuilder, default_symbol: typing.SupportsInt | typing.SupportsIndex = 0) -> Stats:
        ...
    def ts(self, symbol: str = '') -> numpy.typing.NDArray[numpy.int64]:
        ...
    def volume(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    @property
    def symbols(self) -> list:
        ...
class EventTypeStatsAggregator(_AggregatorHandle):
    """
    Per-symbol counters split by event kind (trade / book_snapshot / book_delta). Cheapest of the native aggregators; useful as the 'what's in this tape' overview.
    """
    def __init__(self, event_filter: AggregatorEventFilter = ..., symbol_filter: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> None:
        ...
    def result(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Structured numpy array with fields (symbol_id u4, trades u8, book_snapshots u8, book_deltas u8). Sorted by symbol_id ascending. Empty until DataReader.run() has called finalize().
        """
class ExchangeCapabilities:
    batch_submit: bool
    close_position: bool
    fok: bool
    gtc: bool
    gtd: bool
    iceberg: bool
    ioc: bool
    oco: bool
    post_only: bool
    reduce_only: bool
    stop_limit: bool
    stop_market: bool
    take_profit_limit: bool
    take_profit_market: bool
    trailing_stop: bool
    def __init__(self) -> None:
        ...
class ExecutionListener:
    def __init__(self) -> None:
        ...
    def on_accepted(self, arg0: Order) -> None:
        ...
    def on_canceled(self, arg0: Order) -> None:
        ...
    def on_expired(self, arg0: Order) -> None:
        ...
    def on_filled(self, arg0: Order) -> None:
        ...
    def on_partially_filled(self, order: Order, fill_qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def on_pending_cancel(self, arg0: Order) -> None:
        ...
    def on_pending_trigger(self, arg0: Order) -> None:
        ...
    def on_rejected(self, order: Order, reason: str) -> None:
        ...
    def on_replaced(self, old_order: Order, new_order: Order) -> None:
        ...
    def on_submitted(self, arg0: Order) -> None:
        ...
    def on_trailing_stop_updated(self, order: Order, new_trigger: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def on_triggered(self, arg0: Order) -> None:
        ...
class Executor:
    def __init__(self) -> None:
        ...
    def cancel(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def cancel_all(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def capabilities(self) -> ExchangeCapabilities:
        ...
    def on_start(self) -> None:
        ...
    def on_stop(self) -> None:
        ...
    def replace(self, old_order_id: typing.SupportsInt | typing.SupportsIndex, new_order: Order) -> None:
        ...
    def submit(self, order: Order) -> None:
        ...
    def submit_oco(self, order1: Order, order2: Order) -> None:
        ...
class ExponentialLatency(LatencyModel):
    """
    Exponential per component, parameterised by mean. Heavy right tail by default.
    """
    def __init__(self, feed_mean_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, order_mean_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, fill_mean_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, seed: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
class FeeSchedule:
    @staticmethod
    def binance_um_futures() -> FeeSchedule:
        ...
    @staticmethod
    def bybit_linear() -> FeeSchedule:
        ...
    @staticmethod
    def deribit() -> FeeSchedule:
        ...
    @staticmethod
    def okx_swap() -> FeeSchedule:
        ...
    def __init__(self) -> None:
        ...
    def add_tier(self, min_notional_30d: typing.SupportsFloat | typing.SupportsIndex, maker_bps: typing.SupportsFloat | typing.SupportsIndex, taker_bps: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def bind_account(self, account: Account) -> None:
        """
        Bind an Account so 30d rolling notional reads from the account's aggregate counter instead of this schedule's internal one.
        """
    def clear_account_binding(self) -> None:
        ...
    def current_bps(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> tuple[float, float]:
        ...
    def current_tier_index(self) -> int:
        ...
    def fee_for(self, ts_ns: typing.SupportsInt | typing.SupportsIndex, notional: typing.SupportsFloat | typing.SupportsIndex, is_maker: bool) -> float:
        ...
    def record_fill(self, ts_ns: typing.SupportsInt | typing.SupportsIndex, notional: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def reset_rolling(self) -> None:
        ...
    def rolling_notional_30d(self) -> float:
        ...
    def tier_count(self) -> int:
        ...
    def tier_transition_ts_ns(self) -> list[int]:
        ...
    def tiers(self) -> list[FeeTier]:
        ...
class FeeTier:
    @property
    def maker_bps(self) -> float:
        ...
    @property
    def min_notional_30d(self) -> float:
        ...
    @property
    def taker_bps(self) -> float:
        ...
class FeedClockPolicy:
    """
    Members:
    
      WAIT_FOR_ALL
    
      FIRE_ON_ANY
    
      LEADER_FOLLOWER
    """
    FIRE_ON_ANY: typing.ClassVar[FeedClockPolicy]  # value = <FeedClockPolicy.FIRE_ON_ANY: 1>
    LEADER_FOLLOWER: typing.ClassVar[FeedClockPolicy]  # value = <FeedClockPolicy.LEADER_FOLLOWER: 2>
    WAIT_FOR_ALL: typing.ClassVar[FeedClockPolicy]  # value = <FeedClockPolicy.WAIT_FOR_ALL: 0>
    __members__: typing.ClassVar[dict[str, FeedClockPolicy]]  # value = {'WAIT_FOR_ALL': <FeedClockPolicy.WAIT_FOR_ALL: 0>, 'FIRE_ON_ANY': <FeedClockPolicy.FIRE_ON_ANY: 1>, 'LEADER_FOLLOWER': <FeedClockPolicy.LEADER_FOLLOWER: 2>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class FillLiquidity:
    """
    Members:
    
      UNKNOWN
    
      MAKER
    
      TAKER
    """
    MAKER: typing.ClassVar[FillLiquidity]  # value = <FillLiquidity.MAKER: 1>
    TAKER: typing.ClassVar[FillLiquidity]  # value = <FillLiquidity.TAKER: 2>
    UNKNOWN: typing.ClassVar[FillLiquidity]  # value = <FillLiquidity.UNKNOWN: 0>
    __members__: typing.ClassVar[dict[str, FillLiquidity]]  # value = {'UNKNOWN': <FillLiquidity.UNKNOWN: 0>, 'MAKER': <FillLiquidity.MAKER: 1>, 'TAKER': <FillLiquidity.TAKER: 2>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class FootprintBar:
    """
    Footprint bar for order flow analysis. Tracks bid/ask volume at each price level.
    """
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def add_trade(self, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        ...
    def add_trades(self, prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> None:
        ...
    def clear(self) -> None:
        ...
    def highest_buying_pressure(self) -> float:
        ...
    def highest_selling_pressure(self) -> float:
        ...
    def levels(self) -> list:
        ...
    def num_levels(self) -> int:
        ...
    def strongest_imbalance(self, threshold: typing.SupportsFloat | typing.SupportsIndex = 0.7) -> typing.Any:
        ...
    def total_delta(self) -> float:
        ...
    def total_volume(self) -> float:
        ...
class FundingPayment:
    @property
    def amount(self) -> float:
        ...
    @property
    def mark_price(self) -> float:
        ...
    @property
    def position_signed(self) -> float:
        ...
    @property
    def rate(self) -> float:
        ...
    @property
    def symbol(self) -> int:
        ...
    @property
    def timestamp_ns(self) -> int:
        ...
class FundingSchedule:
    @staticmethod
    def binance_um_futures() -> FundingSchedule:
        ...
    @staticmethod
    def bitget_hourly() -> FundingSchedule:
        ...
    @staticmethod
    def bybit_linear() -> FundingSchedule:
        ...
    @staticmethod
    def constant(interval_ns: typing.SupportsInt | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex) -> FundingSchedule:
        ...
    @staticmethod
    def okx_swap() -> FundingSchedule:
        ...
    @staticmethod
    def tape(events: collections.abc.Sequence[tuple[typing.SupportsInt | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]]) -> FundingSchedule:
        ...
    @staticmethod
    def tape_by_symbol(entries: collections.abc.Sequence[FundingTapeEntry]) -> FundingSchedule:
        ...
    def __init__(self) -> None:
        ...
    def constant_rate(self) -> float:
        ...
    def interval_ns(self) -> int:
        ...
    def last_tick_ns(self) -> int:
        ...
    def load_tape(self, path: str) -> bool:
        """
        Read CSV with columns timestamp_ns, symbol, funding_rate. Returns True on success.
        """
    def per_symbol_tape(self) -> list[FundingTapeEntry]:
        ...
    def reset(self) -> None:
        ...
    def set_constant_rate(self, rate: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def settlement_timestamps(self) -> list[int]:
        ...
    def tick(self, now_ns: typing.SupportsInt | typing.SupportsIndex, symbols: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex], positions: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], mark_prices: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex]) -> list[FundingPayment]:
        ...
class FundingTapeEntry:
    def __init__(self) -> None:
        ...
    @property
    def rate(self) -> float:
        ...
    @rate.setter
    def rate(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def timestamp_ns(self) -> int:
        ...
    @timestamp_ns.setter
    def timestamp_ns(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class GaussianLatency(LatencyModel):
    """
    Independent normal samples per component, clamped to non-negative. Stddev <= 0 collapses the component to a deterministic mean.
    """
    def __init__(self, feed_mean_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, feed_stddev_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, order_mean_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, order_stddev_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, fill_mean_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, fill_stddev_ns: typing.SupportsFloat | typing.SupportsIndex = 0.0, seed: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
class GridSearch:
    def __init__(self) -> None:
        ...
    def add_axis(self, values: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex]) -> None:
        """
        Append an axis of parameter values. Total combinations is the product of axis lengths; the last axis varies fastest.
        """
    def params_for_index(self, index: typing.SupportsInt | typing.SupportsIndex) -> list[float]:
        ...
    def run(self) -> list:
        """
        Run the grid sequentially. Returns a list of dicts with keys 'index', 'params', 'stats'.
        """
    def set_factory(self, factory: typing.Any) -> None:
        """
        Callable[[List[float]], Dict[str, Any]] — receives one parameter point in axis order, returns a stats dict (the shape returned by BacktestRunner.run_csv).
        """
    def total(self) -> int:
        ...
class IndicatorGraph:
    def __init__(self) -> None:
        ...
    def add_node(self, name: str, deps: collections.abc.Sequence[str], fn: typing.Any) -> None:
        ...
    def bar_count(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def close(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def current(self, symbol: typing.SupportsInt | typing.SupportsIndex, name: str) -> float:
        ...
    def get(self, symbol: typing.SupportsInt | typing.SupportsIndex, name: str) -> typing.Any:
        ...
    def high(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def indicator(self, name: str, indicator: typing.Any, source: str = 'close') -> None:
        """
        Add a node that runs `indicator.compute(graph.<source>(sym))`. Sugar over add_node.
        """
    def invalidate(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def invalidate_all(self) -> None:
        ...
    def low(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def require(self, symbol: typing.SupportsInt | typing.SupportsIndex, name: str) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def reset_all(self) -> None:
        ...
    def set_bars(self, symbol: typing.SupportsInt | typing.SupportsIndex, close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64] | None = None, low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64] | None = None, volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64] | None = None) -> None:
        ...
    def step(self, symbol: typing.SupportsInt | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex, high: typing.SupportsFloat | typing.SupportsIndex | None = None, low: typing.SupportsFloat | typing.SupportsIndex | None = None, volume: typing.SupportsFloat | typing.SupportsIndex | None = None) -> None:
        ...
    def volume(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
class KAMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex, fast: typing.SupportsInt | typing.SupportsIndex = 2, slow: typing.SupportsInt | typing.SupportsIndex = 30) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class KillSwitch:
    def __init__(self) -> None:
        ...
    def check(self, signal: Signal) -> bool:
        ...
class Kurtosis:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class L3Book:
    def __init__(self) -> None:
        ...
    def add_order(self, order_id: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, side: str) -> str:
        ...
    def ask_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def best_ask(self) -> typing.Any:
        ...
    def best_bid(self) -> typing.Any:
        ...
    def bid_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def build_from_snapshot(self, orders: list) -> None:
        ...
    def export_snapshot(self) -> list:
        ...
    def modify_order(self, order_id: typing.SupportsInt | typing.SupportsIndex, new_quantity: typing.SupportsFloat | typing.SupportsIndex) -> str:
        ...
    def remove_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> str:
        ...
class LatencyDistribution:
    @staticmethod
    def constant(ns: typing.SupportsInt | typing.SupportsIndex) -> LatencyDistribution:
        ...
    @staticmethod
    def empirical(samples_ns: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex]) -> LatencyDistribution:
        ...
    @staticmethod
    def lognormal(median_ns: typing.SupportsInt | typing.SupportsIndex, sigma: typing.SupportsFloat | typing.SupportsIndex) -> LatencyDistribution:
        ...
    @staticmethod
    def uniform(lo_ns: typing.SupportsInt | typing.SupportsIndex, hi_ns: typing.SupportsInt | typing.SupportsIndex) -> LatencyDistribution:
        ...
    def __init__(self) -> None:
        ...
    def median_ns(self) -> int:
        ...
    def set_burst_correlation(self, rho: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def burst_correlation(self) -> float:
        ...
    @burst_correlation.setter
    def burst_correlation(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class LatencyModel:
    """
    Abstract sampler. Subclasses implement feed_delay / order_delay / fill_delay returning non-negative nanoseconds.
    """
    def __init__(self) -> None:
        ...
    def feed_delay(self) -> int:
        ...
    def fill_delay(self) -> int:
        ...
    def order_delay(self) -> int:
        ...
    def reset(self, seed: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        """
        Re-seed the underlying RNG. No-op for deterministic models.
        """
    def sample(self) -> LatencySample:
        """
        Composite draw. Returns a LatencySample.
        """
class LatencySample:
    """
    One draw from a LatencyModel covering feed, order, and fill in non-negative nanoseconds.
    """
    @typing.overload
    def __init__(self) -> None:
        ...
    @typing.overload
    def __init__(self, feed_ns: typing.SupportsInt | typing.SupportsIndex = 0, order_ns: typing.SupportsInt | typing.SupportsIndex = 0, fill_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def to_dict(self) -> dict:
        ...
    @property
    def feed_ns(self) -> int:
        ...
    @feed_ns.setter
    def feed_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def fill_ns(self) -> int:
        ...
    @fill_ns.setter
    def fill_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def order_ns(self) -> int:
        ...
    @order_ns.setter
    def order_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class LegState:
    """
    Members:
    
      PENDING
    
      SUBMITTED
    
      PARTIALLY_FILLED
    
      FILLED
    
      CANCELLED
    
      FAILED
    """
    CANCELLED: typing.ClassVar[LegState]  # value = <LegState.CANCELLED: 4>
    FAILED: typing.ClassVar[LegState]  # value = <LegState.FAILED: 5>
    FILLED: typing.ClassVar[LegState]  # value = <LegState.FILLED: 3>
    PARTIALLY_FILLED: typing.ClassVar[LegState]  # value = <LegState.PARTIALLY_FILLED: 2>
    PENDING: typing.ClassVar[LegState]  # value = <LegState.PENDING: 0>
    SUBMITTED: typing.ClassVar[LegState]  # value = <LegState.SUBMITTED: 1>
    __members__: typing.ClassVar[dict[str, LegState]]  # value = {'PENDING': <LegState.PENDING: 0>, 'SUBMITTED': <LegState.SUBMITTED: 1>, 'PARTIALLY_FILLED': <LegState.PARTIALLY_FILLED: 2>, 'FILLED': <LegState.FILLED: 3>, 'CANCELLED': <LegState.CANCELLED: 4>, 'FAILED': <LegState.FAILED: 5>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class LiquidationEngine:
    @staticmethod
    def binance_um_futures() -> LiquidationEngine:
        ...
    @staticmethod
    def bybit_linear() -> LiquidationEngine:
        ...
    @staticmethod
    def okx_swap() -> LiquidationEngine:
        ...
    def __init__(self) -> None:
        ...
    def account_count(self) -> int:
        ...
    def add_tier(self, min_notional: typing.SupportsFloat | typing.SupportsIndex, mm_fraction: typing.SupportsFloat | typing.SupportsIndex) -> LiquidationEngine:
        ...
    def adl_closeouts_count(self) -> int:
        ...
    def adl_enabled(self) -> bool:
        ...
    def adl_ranking(self) -> AdlRanking:
        ...
    def attach_account(self, account: Account) -> None:
        """
        Attach an Account so its positions participate in cross-margin MM checks. Multiple accounts may be attached.
        """
    def cascade_sizes_per_tick(self) -> list[int]:
        ...
    def close_position(self, account_id: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def deficits_paid_by_adl(self) -> list[float]:
        ...
    def deficits_paid_by_fund(self) -> list[float]:
        ...
    def detach_account(self, account_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def fund_balance_history(self) -> list[float]:
        ...
    def insurance_fund_balance(self) -> float:
        ...
    def insurance_payments_count(self) -> int:
        ...
    def liquidations_count(self) -> int:
        ...
    def mark_impact_model(self) -> MarkImpactModel:
        ...
    def mark_impact_weight(self) -> float:
        ...
    def max_cascade_depth(self) -> int:
        ...
    def on_mark(self, symbol: typing.SupportsInt | typing.SupportsIndex, mark_price: typing.SupportsFloat | typing.SupportsIndex) -> dict:
        ...
    def on_marks(self, marks: collections.abc.Sequence[tuple[typing.SupportsInt | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]], ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> dict:
        """
        Atomically update every attached account's mark for each (symbol, price) pair, then walk liquidations once per symbol. Returns aggregated outcome.
        """
    def open_position(self, account_id: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, entry_price: typing.SupportsFloat | typing.SupportsIndex, equity: typing.SupportsFloat | typing.SupportsIndex, contract_multiplier: typing.SupportsFloat | typing.SupportsIndex = 1.0, is_long_option: bool = False) -> None:
        ...
    def position_count(self) -> int:
        ...
    def reset_stats(self) -> None:
        ...
    def set_adl_enabled(self, enabled: bool) -> None:
        ...
    def set_adl_ranking(self, ranking: typing.Any) -> None:
        """
        Set the ADL queue ordering. Accepts an AdlRanking enum or a string name (pnl_ratio, binance, bybit, position_size).
        """
    def set_executor(self, executor: typing.Any) -> None:
        """
        Attach a SimulatedExecutor so liquidation orders route through it as market orders. Pass None to detach.
        """
    def set_insurance_fund_capital(self, capital: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def set_liquidation_slippage_bps(self, bps: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def set_mark_impact_model(self, model: typing.Any, weight: typing.SupportsFloat | typing.SupportsIndex = 0.3) -> None:
        """
        Configure mark-impact feedback after liquidations. Accepts a MarkImpactModel enum or a string name (none, book_anchored, book_only). `weight` is the blend factor used by book_anchored.
        """
    def set_max_cascade_depth(self, depth: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def ticks_to_first_adl(self) -> int:
        ...
class LiveQueuePositionEstimator:
    def __init__(self) -> None:
        ...
    def on_level_update(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, new_qty: typing.SupportsFloat | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_order_cancelled(self, order_id: typing.SupportsInt | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_order_filled(self, order_id: typing.SupportsInt | typing.SupportsIndex, cumulative_fill: typing.SupportsFloat | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_order_placed(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, order_id: typing.SupportsInt | typing.SupportsIndex, order_qty: typing.SupportsFloat | typing.SupportsIndex, level_qty_now: typing.SupportsFloat | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_trade(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_trade_with_flag(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0, is_hidden: bool = False) -> None:
        ...
    def set_confidence_half_life_ns(self, half_life_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def set_hidden_order_policy(self, policy: str) -> None:
        ...
    def set_shrink_attribution_factor(self, factor: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def snapshot(self, order_id: typing.SupportsInt | typing.SupportsIndex, now_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> typing.Any:
        ...
    def tracked_order_count(self) -> int:
        ...
class LrvxError(Exception):
    pass
class MACD:
    def __init__(self, fast: typing.SupportsInt | typing.SupportsIndex = 12, slow: typing.SupportsInt | typing.SupportsIndex = 26, signal: typing.SupportsInt | typing.SupportsIndex = 9) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> dict:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def histogram(self) -> float | None:
        ...
    @property
    def line(self) -> float | None:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def signal(self) -> float | None:
        ...
    @property
    def value(self) -> float | None:
        ...
class MarginMode:
    """
    Members:
    
      Cross
    
      Isolated
    """
    Cross: typing.ClassVar[MarginMode]  # value = <MarginMode.Cross: 0>
    Isolated: typing.ClassVar[MarginMode]  # value = <MarginMode.Isolated: 1>
    __members__: typing.ClassVar[dict[str, MarginMode]]  # value = {'Cross': <MarginMode.Cross: 0>, 'Isolated': <MarginMode.Isolated: 1>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class MarkImpactModel:
    """
    Members:
    
      None_
    
      BookAnchored
    
      BookOnly
    """
    BookAnchored: typing.ClassVar[MarkImpactModel]  # value = <MarkImpactModel.BookAnchored: 1>
    BookOnly: typing.ClassVar[MarkImpactModel]  # value = <MarkImpactModel.BookOnly: 2>
    None_: typing.ClassVar[MarkImpactModel]  # value = <MarkImpactModel.None_: 0>
    __members__: typing.ClassVar[dict[str, MarkImpactModel]]  # value = {'None_': <MarkImpactModel.None_: 0>, 'BookAnchored': <MarkImpactModel.BookAnchored: 1>, 'BookOnly': <MarkImpactModel.BookOnly: 2>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class MarketDataRecorderHook:
    def __init__(self) -> None:
        ...
    def on_book_update(self, symbol: typing.SupportsInt | typing.SupportsIndex, is_snapshot: bool, bids: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]], asks: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]], ts_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def on_start(self) -> None:
        ...
    def on_stop(self) -> None:
        ...
    def on_trade(self, arg0: TradeData) -> None:
        ...
class MarketProfile:
    """
    Market Profile (TPO) aggregator. Tracks price activity across time periods.
    """
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex, period_minutes: typing.SupportsInt | typing.SupportsIndex, session_start_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def add_trade(self, timestamp_ns: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        ...
    def add_trades(self, timestamps_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> None:
        ...
    def clear(self) -> None:
        ...
    def current_period(self) -> int:
        ...
    def initial_balance_high(self) -> float:
        ...
    def initial_balance_low(self) -> float:
        ...
    def is_poor_high(self) -> bool:
        ...
    def is_poor_low(self) -> bool:
        ...
    def levels(self) -> list:
        ...
    def num_levels(self) -> int:
        ...
    def poc(self) -> float:
        ...
    def single_prints(self) -> list:
        ...
    def value_area_high(self) -> float:
        ...
    def value_area_low(self) -> float:
        ...
class MergedTapeReader:
    def __init__(self, paths: collections.abc.Sequence[str], from_ns: typing.Any = None, to_ns: typing.Any = None, symbols: typing.Any = None) -> None:
        """
        Open N `.lrvx` directories and expose a merged trade / book stream. Symbols are rekeyed into a global id space, keyed by (metadata.exchange, name). Ties on exchange_ts_ns are broken by tape order in `paths`. read_trades / read_books are eager — they materialise the merged arrays.
        """
    def per_tape_stats(self) -> list:
        """
        Per-tape breakdown — useful for debugging an empty input.
        """
    def read_books(self) -> tuple:
        """
        (headers, levels) tuple. Headers carry global symbol_id; level_offset slices the levels array per event.
        """
    def read_trades(self) -> numpy.ndarray:
        """
        Merged trades as PyTrade structured numpy array, sorted by exchange_ts_ns; tie-break by tape order.
        """
    def run(self, aggregators: list) -> bool:
        """
        Run a panel of streaming aggregators over the merged tape stream in a single decompression pass. Same semantics as DataReader.run — events are seen with global-rewritten symbol ids; per-tape provenance is not surfaced to aggregators (reach for stream_events directly when needed).
        """
    def stream_events(self, on_trade: typing.Any = None, on_book: typing.Any = None) -> None:
        """
        Walk the merged stream via N-way heap merge (O(N tapes) peak memory). Calls on_trade(exchange_ts_ns, recv_ts_ns, price_raw, qty_raw, symbol_id, tape_index, side) and on_book(exchange_ts_ns, recv_ts_ns, symbol_id, tape_index, is_snapshot, bids, asks). bids/asks are lists of (price_raw, qty_raw) tuples. Returning False from either aborts the walk.
        """
    def summary(self) -> dict:
        """
        Aggregate stats: first_event_ns, last_event_ns, total_events (populated after readTrades/readBooks), tape_count, symbol_count.
        """
    def symbol_table(self) -> list:
        """
        List of dicts: global_id, exchange, name, price_precision, qty_precision.
        """
    def time_range(self) -> tuple:
        """
        (min first_event_ns, max last_event_ns) across all tapes.
        """
class MultiFeedClock:
    def __init__(self, symbols: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex], policy: FeedClockPolicy = ..., timeout_ms: typing.SupportsInt | typing.SupportsIndex = 200, leader_symbol: typing.SupportsInt | typing.SupportsIndex = 0, staleness_budget_ms: typing.SupportsInt | typing.SupportsIndex = 200) -> None:
        ...
    def policy(self) -> FeedClockPolicy:
        ...
    def reset(self) -> None:
        ...
    def symbol_count(self) -> int:
        ...
    def tick(self, ts_ns: typing.SupportsInt | typing.SupportsIndex, symbol_id: typing.SupportsInt | typing.SupportsIndex) -> dict:
        ...
class OHLCBinAggregator(_AggregatorHandle):
    """
    Time-bucketed price OHLC over trade events. For each cell, records open (earliest ts), close (latest ts), high (max), and low (min) of trade.price_raw. Empty buckets produce no row; forward-fill on the caller side if a dense series is needed. Books are ignored.
    """
    def __init__(self, bucket_ns: typing.SupportsInt | typing.SupportsIndex, by_symbol: bool = False, event_filter: AggregatorEventFilter = ..., symbol_filter: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> None:
        ...
    def result(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Structured numpy: (bucket_ts_ns i8, symbol_id u4, open_raw i8, high_raw i8, low_raw i8, close_raw i8). *_raw fields are fixed-point. Divide by Price::SCALE to get floats. symbol_id is 0 when by_symbol=False.
        """
class OptionType:
    """
    Members:
    
      CALL
    
      PUT
    """
    CALL: typing.ClassVar[OptionType]  # value = <OptionType.CALL: 0>
    PUT: typing.ClassVar[OptionType]  # value = <OptionType.PUT: 1>
    __members__: typing.ClassVar[dict[str, OptionType]]  # value = {'CALL': <OptionType.CALL: 0>, 'PUT': <OptionType.PUT: 1>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class Order:
    close_position: bool
    order_type: str
    post_only: bool
    reduce_only: bool
    side: str
    time_in_force: str
    def __init__(self) -> None:
        ...
    @property
    def client_order_id(self) -> int:
        ...
    @client_order_id.setter
    def client_order_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def created_at_ns(self) -> int:
        ...
    @created_at_ns.setter
    def created_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def exchange_ts_ns(self) -> int:
        ...
    @exchange_ts_ns.setter
    def exchange_ts_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def filled_quantity(self) -> float:
        ...
    @filled_quantity.setter
    def filled_quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def id(self) -> int:
        ...
    @id.setter
    def id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def order_tag(self) -> int:
        ...
    @order_tag.setter
    def order_tag(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def price(self) -> float:
        ...
    @price.setter
    def price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def quantity(self) -> float:
        ...
    @quantity.setter
    def quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def strategy_id(self) -> int:
        ...
    @strategy_id.setter
    def strategy_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def trailing_offset(self) -> float:
        ...
    @trailing_offset.setter
    def trailing_offset(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def trigger_price(self) -> float:
        ...
    @trigger_price.setter
    def trigger_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class OrderBook:
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def apply_delta(self, bid_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], bid_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> None:
        """
        Apply an incremental book update
        """
    def apply_snapshot(self, bid_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], bid_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> None:
        """
        Apply a full book snapshot
        """
    def ask_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def best_ask(self) -> typing.Any:
        """
        Best ask price or None
        """
    def best_bid(self) -> typing.Any:
        """
        Best bid price or None
        """
    def bid_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def clear(self) -> None:
        ...
    def consume_asks(self, quantity: typing.SupportsFloat | typing.SupportsIndex) -> tuple:
        """
        Simulate market buy: returns (filled_qty, total_cost)
        """
    def consume_bids(self, quantity: typing.SupportsFloat | typing.SupportsIndex) -> tuple:
        """
        Simulate market sell: returns (filled_qty, total_cost)
        """
    def get_asks(self, max_levels: typing.SupportsInt | typing.SupportsIndex = 20) -> numpy.typing.NDArray[numpy.float64]:
        """
        Get ask levels as Nx2 array [price, qty]
        """
    def get_bids(self, max_levels: typing.SupportsInt | typing.SupportsIndex = 20) -> numpy.typing.NDArray[numpy.float64]:
        """
        Get bid levels as Nx2 array [price, qty]
        """
    def is_crossed(self) -> bool:
        ...
    def mid(self) -> typing.Any:
        """
        Mid price or None
        """
    def spread(self) -> typing.Any:
        """
        Bid-ask spread or None
        """
class OrderEventData:
    fill_role: str
    is_maker: bool
    market_position: str
    order_type: str
    reject_reason: str
    side: str
    status: str
    @property
    def accepted_at_ns(self) -> int:
        ...
    @accepted_at_ns.setter
    def accepted_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def canceled_at_ns(self) -> int:
        ...
    @canceled_at_ns.setter
    def canceled_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def distance_to_best_ticks(self) -> int:
        ...
    @distance_to_best_ticks.setter
    def distance_to_best_ticks(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def exchange_ts_ns(self) -> int:
        ...
    @exchange_ts_ns.setter
    def exchange_ts_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def expired_at_ns(self) -> int:
        ...
    @expired_at_ns.setter
    def expired_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def fill_price(self) -> float:
        ...
    @fill_price.setter
    def fill_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def fill_qty(self) -> float:
        ...
    @fill_qty.setter
    def fill_qty(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def first_fill_at_ns(self) -> int:
        ...
    @first_fill_at_ns.setter
    def first_fill_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def last_fill_at_ns(self) -> int:
        ...
    @last_fill_at_ns.setter
    def last_fill_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def order_id(self) -> int:
        ...
    @order_id.setter
    def order_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def queue_ahead(self) -> float:
        ...
    @queue_ahead.setter
    def queue_ahead(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def queue_total(self) -> float:
        ...
    @queue_total.setter
    def queue_total(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def rejected_at_ns(self) -> int:
        ...
    @rejected_at_ns.setter
    def rejected_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def submitted_at_ns(self) -> int:
        ...
    @submitted_at_ns.setter
    def submitted_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol_id(self) -> int:
        ...
    @symbol_id.setter
    def symbol_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def triggered_at_ns(self) -> int:
        ...
    @triggered_at_ns.setter
    def triggered_at_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class OrderEventKind:
    """
    Members:
    
      SUBMIT
    
      CANCEL
    
      MODIFY
    
      ACK
    
      REJECT
    
      EXPIRE
    """
    ACK: typing.ClassVar[OrderEventKind]  # value = <OrderEventKind.ACK: 4>
    CANCEL: typing.ClassVar[OrderEventKind]  # value = <OrderEventKind.CANCEL: 2>
    EXPIRE: typing.ClassVar[OrderEventKind]  # value = <OrderEventKind.EXPIRE: 6>
    MODIFY: typing.ClassVar[OrderEventKind]  # value = <OrderEventKind.MODIFY: 3>
    REJECT: typing.ClassVar[OrderEventKind]  # value = <OrderEventKind.REJECT: 5>
    SUBMIT: typing.ClassVar[OrderEventKind]  # value = <OrderEventKind.SUBMIT: 1>
    __members__: typing.ClassVar[dict[str, OrderEventKind]]  # value = {'SUBMIT': <OrderEventKind.SUBMIT: 1>, 'CANCEL': <OrderEventKind.CANCEL: 2>, 'MODIFY': <OrderEventKind.MODIFY: 3>, 'ACK': <OrderEventKind.ACK: 4>, 'REJECT': <OrderEventKind.REJECT: 5>, 'EXPIRE': <OrderEventKind.EXPIRE: 6>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class OrderGroup:
    def __init__(self, parent_signal_id: typing.SupportsInt | typing.SupportsIndex = 0, policy: OrderGroupPolicy = ...) -> None:
        ...
    def add_limit_leg(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def add_market_leg(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def auto_dispatch(self, strategy: typing.Any) -> int:
        ...
    def find_leg_by_order_id(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        ...
    def leg_count(self) -> int:
        ...
    def leg_filled(self, leg_index: typing.SupportsInt | typing.SupportsIndex) -> float:
        ...
    def leg_order_id(self, leg_index: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def leg_state(self, leg_index: typing.SupportsInt | typing.SupportsIndex) -> LegState:
        ...
    def mark_action_dispatched(self, leg_index: typing.SupportsInt | typing.SupportsIndex, kind: str) -> None:
        ...
    def pair_latency_decision(self, leader_submit_ts_ns: typing.SupportsInt | typing.SupportsIndex, leader_ack_ts_ns: typing.SupportsInt | typing.SupportsIndex, ack_received: bool = False) -> str:
        ...
    def parent_signal_id(self) -> int:
        ...
    def policy(self) -> OrderGroupPolicy:
        ...
    def precheck_submission(self, equity: typing.SupportsFloat | typing.SupportsIndex = 0.0, market_ref_prices: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex] = []) -> dict:
        ...
    def recommended_actions(self) -> list:
        ...
    def record_cancel(self, leg_index: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def record_failure(self, leg_index: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def record_fill(self, leg_index: typing.SupportsInt | typing.SupportsIndex, cumulative_qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def record_replace_accepted(self, leg_index: typing.SupportsInt | typing.SupportsIndex, new_order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def record_replace_rejected(self, leg_index: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def record_submit(self, leg_index: typing.SupportsInt | typing.SupportsIndex, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def set_pair_latency_budget_ns(self, budget_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def set_risk_limits(self, max_gross_notional: typing.SupportsFloat | typing.SupportsIndex = 0.0, max_concentration_pct: typing.SupportsFloat | typing.SupportsIndex = 0.0, max_leg_qty: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        ...
    def state(self) -> OrderGroupState:
        ...
class OrderGroupPolicy:
    """
    Members:
    
      BEST_EFFORT
    
      ALL_OR_NOTHING
    
      ONE_SIDED
    """
    ALL_OR_NOTHING: typing.ClassVar[OrderGroupPolicy]  # value = <OrderGroupPolicy.ALL_OR_NOTHING: 1>
    BEST_EFFORT: typing.ClassVar[OrderGroupPolicy]  # value = <OrderGroupPolicy.BEST_EFFORT: 0>
    ONE_SIDED: typing.ClassVar[OrderGroupPolicy]  # value = <OrderGroupPolicy.ONE_SIDED: 2>
    __members__: typing.ClassVar[dict[str, OrderGroupPolicy]]  # value = {'BEST_EFFORT': <OrderGroupPolicy.BEST_EFFORT: 0>, 'ALL_OR_NOTHING': <OrderGroupPolicy.ALL_OR_NOTHING: 1>, 'ONE_SIDED': <OrderGroupPolicy.ONE_SIDED: 2>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class OrderGroupState:
    """
    Members:
    
      PENDING
    
      SUBMITTED
    
      PARTIALLY_FILLED
    
      FILLED
    
      CANCELLED
    
      REVERTING
    
      FAILED
    """
    CANCELLED: typing.ClassVar[OrderGroupState]  # value = <OrderGroupState.CANCELLED: 4>
    FAILED: typing.ClassVar[OrderGroupState]  # value = <OrderGroupState.FAILED: 6>
    FILLED: typing.ClassVar[OrderGroupState]  # value = <OrderGroupState.FILLED: 3>
    PARTIALLY_FILLED: typing.ClassVar[OrderGroupState]  # value = <OrderGroupState.PARTIALLY_FILLED: 2>
    PENDING: typing.ClassVar[OrderGroupState]  # value = <OrderGroupState.PENDING: 0>
    REVERTING: typing.ClassVar[OrderGroupState]  # value = <OrderGroupState.REVERTING: 5>
    SUBMITTED: typing.ClassVar[OrderGroupState]  # value = <OrderGroupState.SUBMITTED: 1>
    __members__: typing.ClassVar[dict[str, OrderGroupState]]  # value = {'PENDING': <OrderGroupState.PENDING: 0>, 'SUBMITTED': <OrderGroupState.SUBMITTED: 1>, 'PARTIALLY_FILLED': <OrderGroupState.PARTIALLY_FILLED: 2>, 'FILLED': <OrderGroupState.FILLED: 3>, 'CANCELLED': <OrderGroupState.CANCELLED: 4>, 'REVERTING': <OrderGroupState.REVERTING: 5>, 'FAILED': <OrderGroupState.FAILED: 6>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class OrderJourneyTracer:
    def __init__(self, max_orders: typing.SupportsInt | typing.SupportsIndex = 1000000, max_records_per_order: typing.SupportsInt | typing.SupportsIndex = 64, sample_rate: typing.SupportsFloat | typing.SupportsIndex = 1.0, sample_salt: typing.SupportsInt | typing.SupportsIndex = 11400714819323198485) -> None:
        ...
    def cancel_race_loss_rate(self) -> float:
        ...
    def clear(self) -> None:
        ...
    def journey(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Return the trace for one order as a numpy structured array.
        """
    def maker_fill_ratio(self) -> float:
        ...
    def median_ack_latency_ns(self) -> float:
        ...
    def median_time_to_first_fill_ns(self) -> float:
        ...
    def order_count(self) -> int:
        ...
    def record_count(self) -> int:
        ...
    def result(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Return the full trace as a numpy structured array, one row per event.
        """
class OrderTracker:
    def __init__(self) -> None:
        ...
    def active_count(self) -> int:
        """
        Number of active orders
        """
    def get(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Get order state dict or None
        """
    def is_active(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Check if order is active
        """
    def on_canceled(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Record order cancellation
        """
    def on_filled(self, order_id: typing.SupportsInt | typing.SupportsIndex, fill_quantity: typing.SupportsFloat | typing.SupportsIndex) -> bool:
        """
        Record order fill
        """
    def on_rejected(self, order_id: typing.SupportsInt | typing.SupportsIndex, reason: str) -> bool:
        """
        Record order rejection
        """
    def on_submitted(self, order_id: typing.SupportsInt | typing.SupportsIndex, exchange_order_id: str, client_order_id: str = '') -> bool:
        """
        Record order submission
        """
    def prune_terminal(self) -> None:
        """
        Remove terminal orders from memory
        """
    def total_count(self) -> int:
        """
        Total tracked orders
        """
class OrderValidator:
    def __init__(self) -> None:
        ...
    def validate(self, signal: Signal) -> bool:
        ...
class ParkinsonVol:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Partitioner:
    def __init__(self, data_dir: str) -> None:
        ...
    def partition_by_calendar(self, unit: str, warmup_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
        ...
    def partition_by_duration(self, duration_ns: typing.SupportsInt | typing.SupportsIndex, warmup_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
        ...
    def partition_by_event_count(self, num_partitions: typing.SupportsInt | typing.SupportsIndex) -> list:
        ...
    def partition_by_symbol(self, num_partitions: typing.SupportsInt | typing.SupportsIndex) -> list:
        ...
    def partition_by_time(self, num_partitions: typing.SupportsInt | typing.SupportsIndex, warmup_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
        ...
    def partition_per_symbol(self) -> list:
        ...
class PeakAggregator(_AggregatorHandle):
    """
    Streaming top-N busiest fixed-width windows per scale. For each window_ns in the constructor list, finds the top_n time intervals that pack the most events into a window of that width. Peaks within 3*window_ns of a stronger one are deduped at finalize().
    """
    def __init__(self, window_ns_list: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex], top_n: typing.SupportsInt | typing.SupportsIndex = 10, oversample_factor: typing.SupportsInt | typing.SupportsIndex = 100, event_filter: AggregatorEventFilter = ..., symbol_filter: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> None:
        ...
    def result(self) -> dict:
        """
        dict[window_ns -> list[(count, start_ns)]] — peaks per scale, sorted by count descending after dedup.
        """
class PnLTracker:
    def __init__(self) -> None:
        ...
    def on_signal(self, signal: Signal) -> None:
        ...
class PoolReplay:
    def curve(self) -> AmmCurve:
        ...
    def drift_count(self) -> int:
        ...
    def trade_count(self) -> int:
        ...
class PoolTape:
    def __init__(self) -> None:
        ...
    def checkpoint(self, ts_ns: typing.SupportsInt | typing.SupportsIndex, reserve0: int, reserve1: int) -> None:
        ...
    def checkpoint_clmm(self, ts_ns: typing.SupportsInt | typing.SupportsIndex, sqrt_price: int, liquidity: int, ticks: list = []) -> None:
        ...
    def descriptor_clmm(self, venue: typing.SupportsInt | typing.SupportsIndex, fee_pips: typing.SupportsInt | typing.SupportsIndex, base_dec: typing.SupportsInt | typing.SupportsIndex, quote_dec: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def descriptor_constant_product(self, fee_num: typing.SupportsInt | typing.SupportsIndex, fee_den: typing.SupportsInt | typing.SupportsIndex, base_dec: typing.SupportsInt | typing.SupportsIndex, quote_dec: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def descriptor_raydium_cp(self, trade_fee_rate: typing.SupportsInt | typing.SupportsIndex, creator_fee_rate: typing.SupportsInt | typing.SupportsIndex, creator_fee_on_input: bool, base_dec: typing.SupportsInt | typing.SupportsIndex, quote_dec: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def replay(self, base_idx: typing.SupportsInt | typing.SupportsIndex = 0, quote_idx: typing.SupportsInt | typing.SupportsIndex = 1, base_dec: typing.SupportsInt | typing.SupportsIndex = 18, quote_dec: typing.SupportsInt | typing.SupportsIndex = 18) -> PoolReplay:
        ...
    def swap(self, ts_ns: typing.SupportsInt | typing.SupportsIndex, base_for_quote: bool, amount_in: int) -> None:
        ...
class PositionGroupTracker:
    def __init__(self) -> None:
        ...
    def assign_to_group(self, position_id: typing.SupportsInt | typing.SupportsIndex, group_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Assign a position to a group
        """
    def close_position(self, position_id: typing.SupportsInt | typing.SupportsIndex, exit_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Close a position at exit price
        """
    def create_group(self, parent_id: typing.SupportsInt | typing.SupportsIndex = 0) -> int:
        """
        Create a position group, returns group id
        """
    def get_position(self, position_id: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Get position dict or None
        """
    def group_net_position(self, group_id: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Net position for a group
        """
    def group_realized_pnl(self, group_id: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Realized PnL for a group
        """
    def group_unrealized_pnl(self, group_id: typing.SupportsInt | typing.SupportsIndex, current_price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        """
        Unrealized PnL for a group at current price
        """
    def net_position(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Net position for symbol
        """
    def open_position(self, order_id: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        """
        Open a new individual position, returns position id
        """
    def open_position_count(self, symbol: typing.Any = None) -> int:
        """
        Count of open positions (optionally filtered by symbol)
        """
    def open_positions(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> list:
        """
        List of open position dicts for symbol
        """
    def partial_close(self, position_id: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, exit_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Partially close a position
        """
    def prune_closed(self) -> None:
        """
        Remove closed positions from memory
        """
    def realized_pnl(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Realized PnL for symbol
        """
    def total_realized_pnl(self) -> float:
        """
        Total realized PnL across all symbols
        """
class PositionTracker:
    def __init__(self, cost_basis: str = 'fifo') -> None:
        ...
    def avg_entry_price(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Average entry price for symbol
        """
    def on_fill(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Record a fill
        """
    def position(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Signed position for symbol
        """
    def realized_pnl(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Realized PnL for symbol
        """
    def total_realized_pnl(self) -> float:
        """
        Total realized PnL across all symbols
        """
class QuantileAggregator(_AggregatorHandle):
    """
    Window-count distribution + quantile lookup. For each window_ns, observes the count of events inside a sliding window of that width at every event arrival, builds a histogram, and at finalize() resolves each requested quantile to the count threshold below which that fraction of observations lies.
    """
    def __init__(self, window_ns_list: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex], quantiles: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], event_filter: AggregatorEventFilter = ..., symbol_filter: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> None:
        ...
    def result(self) -> dict:
        """
        dict[window_ns -> dict[quantile -> count]].
        """
class RMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class RSI:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class RateLimitEndpointFamily:
    """
    Members:
    
      Trading
    
      MarketData
    
      Account
    """
    Account: typing.ClassVar[RateLimitEndpointFamily]  # value = <RateLimitEndpointFamily.Account: 2>
    MarketData: typing.ClassVar[RateLimitEndpointFamily]  # value = <RateLimitEndpointFamily.MarketData: 1>
    Trading: typing.ClassVar[RateLimitEndpointFamily]  # value = <RateLimitEndpointFamily.Trading: 0>
    __members__: typing.ClassVar[dict[str, RateLimitEndpointFamily]]  # value = {'Trading': <RateLimitEndpointFamily.Trading: 0>, 'MarketData': <RateLimitEndpointFamily.MarketData: 1>, 'Account': <RateLimitEndpointFamily.Account: 2>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class RateLimitPolicy:
    @staticmethod
    def binance_um_futures() -> RateLimitPolicy:
        ...
    @staticmethod
    def bybit_linear() -> RateLimitPolicy:
        ...
    @staticmethod
    def deribit() -> RateLimitPolicy:
        ...
    @staticmethod
    def okx_swap() -> RateLimitPolicy:
        ...
    def __init__(self) -> None:
        ...
    def add_bucket(self, name: str, window_ns: typing.SupportsInt | typing.SupportsIndex, capacity: typing.SupportsInt | typing.SupportsIndex, submit_weight: typing.SupportsInt | typing.SupportsIndex = 1, cancel_weight: typing.SupportsInt | typing.SupportsIndex = 1, replace_weight: typing.SupportsInt | typing.SupportsIndex = 2, family: RateLimitEndpointFamily = ..., query_weight: typing.SupportsInt | typing.SupportsIndex = 1) -> None:
        ...
    def add_family_bucket(self, family: RateLimitEndpointFamily, name: str, window_ns: typing.SupportsInt | typing.SupportsIndex, capacity: typing.SupportsInt | typing.SupportsIndex, query_weight: typing.SupportsInt | typing.SupportsIndex = 1) -> None:
        ...
    def ban_until_ns(self) -> int:
        ...
    def bucket_count(self) -> int:
        ...
    def bucket_states(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> list:
        ...
    def consecutive_rejects(self) -> int:
        ...
    def set_ban(self, after_consecutive_rejects: typing.SupportsInt | typing.SupportsIndex, ban_duration_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def try_consume(self, action: str, now_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
class ReplayEvent:
    trade_is_buy: bool
    type: str
    def __init__(self) -> None:
        ...
    @property
    def asks(self) -> list[tuple[float, float]]:
        ...
    @asks.setter
    def asks(self, arg0: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]]) -> None:
        ...
    @property
    def bids(self) -> list[tuple[float, float]]:
        ...
    @bids.setter
    def bids(self, arg0: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]]) -> None:
        ...
    @property
    def book_symbol(self) -> int:
        ...
    @book_symbol.setter
    def book_symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def timestamp_ns(self) -> int:
        ...
    @timestamp_ns.setter
    def timestamp_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def trade_price(self) -> float:
        ...
    @trade_price.setter
    def trade_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def trade_quantity(self) -> float:
        ...
    @trade_quantity.setter
    def trade_quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def trade_symbol(self) -> int:
        ...
    @trade_symbol.setter
    def trade_symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class ReplaySource:
    def __init__(self) -> None:
        ...
    def next(self) -> ReplayEvent | None:
        ...
    def on_start(self) -> None:
        ...
    def on_stop(self) -> None:
        ...
    def seek_to(self, ts_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
class RiskManager:
    def __init__(self) -> None:
        ...
    def allow(self, signal: Signal) -> bool:
        ...
class RogersSatchellVol:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg3: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, open: typing.SupportsFloat | typing.SupportsIndex, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class RollingZScore:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Runner:
    def __init__(self, registry: SymbolRegistry, on_signal: typing.Any, threaded: bool = False) -> None:
        ...
    def add_strategy(self, strategy: Strategy) -> None:
        ...
    def attach_trace_recorder(self, recorder: typing.Any) -> None:
        ...
    def on_bar(self, symbol: typing.Any, open: typing.SupportsFloat | typing.SupportsIndex, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex, volume: typing.SupportsFloat | typing.SupportsIndex = 0.0, buy_volume: typing.SupportsFloat | typing.SupportsIndex = 0.0, start_time_ns: typing.SupportsInt | typing.SupportsIndex = 0, end_time_ns: typing.SupportsInt | typing.SupportsIndex = 0, bar_type: typing.SupportsInt | typing.SupportsIndex = 0, bar_type_param: typing.SupportsInt | typing.SupportsIndex = 0, close_reason: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_book_snapshot(self, symbol: typing.Any, bid_prices: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], bid_qtys: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], ask_prices: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], ask_qtys: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_trade(self, symbol: typing.Any, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def replace_strategy(self, index: typing.SupportsInt | typing.SupportsIndex, strategy: Strategy) -> None:
        """
        Atomically swap the strategy instance at `index` for a new one. The old strategy's on_stop fires, the bridge's internal state survives, the new strategy's on_start fires afterwards. WebSocket / gRPC connections are unaffected.
        """
    def set_executor(self, executor: Executor) -> None:
        ...
    def set_kill_switch(self, ks: KillSwitch) -> None:
        ...
    @typing.overload
    def set_market_data_recorder(self, recorder: MarketDataRecorderHook) -> None:
        ...
    @typing.overload
    def set_market_data_recorder(self, recorder: BinaryLogRecorderHook) -> None:
        ...
    def set_order_validator(self, ov: OrderValidator) -> None:
        ...
    def set_pnl_tracker(self, tracker: PnLTracker) -> None:
        ...
    def set_risk_manager(self, rm: RiskManager) -> None:
        ...
    def set_storage_sink(self, sink: StorageSink) -> None:
        ...
    def set_trace_feed_ts_ns(self, feed_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def start(self) -> None:
        ...
    def stop(self) -> None:
        ...
    def trace_fill(self, order_id: typing.SupportsInt | typing.SupportsIndex, fill_id: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, fee: typing.SupportsFloat | typing.SupportsIndex, symbol_id: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex, liquidity: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def trace_order_event(self, order_id: typing.SupportsInt | typing.SupportsIndex, parent_signal_id: typing.SupportsInt | typing.SupportsIndex, symbol_id: typing.SupportsInt | typing.SupportsIndex, event_kind: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex, order_type: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, flags: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
class SMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class ShannonEntropy:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex, bins: typing.SupportsInt | typing.SupportsIndex = 10) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Signal:
    order_type: str
    side: str
    def __init__(self) -> None:
        ...
    @property
    def liquidity(self) -> float:
        ...
    @liquidity.setter
    def liquidity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def new_price(self) -> float:
        ...
    @new_price.setter
    def new_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def new_quantity(self) -> float:
        ...
    @new_quantity.setter
    def new_quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def order_id(self) -> int:
        ...
    @order_id.setter
    def order_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def price(self) -> float:
        ...
    @price.setter
    def price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def quantity(self) -> float:
        ...
    @quantity.setter
    def quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def range_lower(self) -> float:
        ...
    @range_lower.setter
    def range_lower(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def range_upper(self) -> float:
        ...
    @range_upper.setter
    def range_upper(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def trailing_bps(self) -> int:
        ...
    @trailing_bps.setter
    def trailing_bps(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def trailing_offset(self) -> float:
        ...
    @trailing_offset.setter
    def trailing_offset(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def trigger_price(self) -> float:
        ...
    @trigger_price.setter
    def trigger_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class SignalBuilder:
    def __init__(self) -> None:
        ...
    def __len__(self) -> int:
        ...
    def buy(self, ts: typing.SupportsInt | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
    def clear(self) -> None:
        ...
    def limit_buy(self, ts: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
    def limit_sell(self, ts: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
    def sell(self, ts: typing.SupportsInt | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
class SimulatedClock:
    """
    Simulated time source shared by a VenueStack. Monotonic: advance_to()
    with a timestamp at or before the current one is a no-op. Use reset()
    to move backwards.
    """
    def advance_to(self, ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Advance to `ns`. Ignored if `ns` is not ahead of the current time.
        """
    def now_ns(self) -> int:
        """
        Current simulated time in nanoseconds.
        """
    def reset(self, ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        """
        Set the clock to `ns`, backwards included.
        """
class SimulatedExecutor:
    def __init__(self) -> None:
        ...
    def advance_clock(self, timestamp_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Advance simulation clock to timestamp
        """
    def apply_latency_profile(self, name: str) -> None:
        """
        Apply a named latency profile: binance_um_futures, bybit_linear, okx_swap, deribit, idealized, adversarial.
        """
    def begin_bar_callback_window(self) -> None:
        """
        Open the bar-callback window: every order submitted while it is open is held instead of matched immediately, and released at the next bar's open. Call before invoking a strategy's bar callback by hand; always pair with end_bar_callback_window.
        """
    def bracket_state(self, bracket_id: typing.SupportsInt | typing.SupportsIndex) -> str:
        """
        Bracket state: pending_entry | entry_filled | tp_filled | stop_filled | canceled.
        """
    def cancel_all(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def cancel_bracket(self, bracket_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def cancel_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def clear_rate_limit_policy(self) -> None:
        ...
    def end_bar_callback_window(self) -> None:
        """
        Close the bar-callback window opened by begin_bar_callback_window.
        """
    def fills(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Get all fills as numpy structured array
        """
    def fills_list(self) -> list:
        """
        Get all fills as list of dicts
        """
    def fok_mode(self) -> str:
        ...
    def iceberg_hidden_remaining_raw(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> int:
        """
        Remaining hidden quantity (raw fixed-point) for an iceberg order.
        """
    def on_bar(self, symbol: typing.SupportsInt | typing.SupportsIndex, close_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Feed a bar close price for order matching
        """
    def on_bar_ohlc(self, symbol: typing.SupportsInt | typing.SupportsIndex, open: typing.SupportsFloat | typing.SupportsIndex, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Feed a full OHLC bar: moves the market to the open (releasing any order held from the previous bar's callback there), then walks low -> high -> close so resting stops/targets match the intrabar extremes. Use this, not on_bar, when driving the executor by hand to reach the fills BacktestRunner.run_bars produces.
        """
    def on_best_levels(self, symbol: typing.SupportsInt | typing.SupportsIndex, bid_price: typing.SupportsFloat | typing.SupportsIndex, bid_qty: typing.SupportsFloat | typing.SupportsIndex, ask_price: typing.SupportsFloat | typing.SupportsIndex, ask_qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Feed a top-of-book snapshot (both best bid and best ask in one call)
        """
    def on_book_snapshot(self, symbol: typing.SupportsInt | typing.SupportsIndex, bid_levels: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]], ask_levels: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]]) -> None:
        """
        Feed a full L2 snapshot. bid_levels and ask_levels are lists of (price, qty) tuples
        """
    def on_trade(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        """
        Feed a trade for order matching
        """
    def on_trade_qty(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        """
        Feed a trade with quantity (enables queue-fill simulation)
        """
    def reset(self) -> None:
        """
        Drop fills and run-scoped state while keeping installed configuration (slippage, queue model, latency, ...), so a second hand-driven run reports that run and not the sum of every run.
        """
    def set_bracket_child_arm_mode(self, mode: str) -> None:
        """
        Child-arm policy: 'on_full_fill' (default) arms TP+stop once on full entry fill; 'on_partial_fill' arms / resizes children incrementally on every partial.
        """
    def set_cancel_ack_latency(self, latency_ns: typing.SupportsInt | typing.SupportsIndex, jitter_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def set_cancel_ack_latency_distribution(self, dist: LatencyDistribution) -> None:
        ...
    def set_default_slippage(self, model: str, ticks: typing.SupportsInt | typing.SupportsIndex = 0, tick_size: typing.SupportsFloat | typing.SupportsIndex = 0.0, bps: typing.SupportsFloat | typing.SupportsIndex = 0.0, impact_coeff: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        """
        Configure default slippage. model: none|fixed_ticks|fixed_bps|volume_impact. tick_size is in price units (0.0 falls back to one raw price unit).
        """
    def set_fok_mode(self, mode: str) -> None:
        """
        FOK fill semantic: any_price (default) or single_price.
        """
    def set_iceberg_jitter_seed(self, seed: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Seed the size-jitter RNG for reproducible refresh sequences.
        """
    def set_iceberg_priority_mode(self, mode: str) -> None:
        """
        Queue priority on refresh: 'back' (default) or 'retain' (CME-style).
        """
    def set_iceberg_refresh_latency(self, latency_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Default refresh latency (ns) between a visible tranche filling and the next one being exposed. 0 = instant (most venues).
        """
    def set_iceberg_size_randomisation_pct(self, pct: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Per-refresh size jitter as a fraction (0.0 deterministic, 0.10 = ±10% uniform).
        """
    def set_lmm_bonus_multiplier(self, multiplier: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        TOP_PRO_LMM: LMM bonus multiplier on tail pro-rata distribution.
        """
    def set_lmm_orders(self, ids: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex]) -> None:
        """
        TOP_PRO_LMM: mark these order ids as Lead Market Makers.
        """
    def set_order_priority_multiplier(self, order_id: typing.SupportsInt | typing.SupportsIndex, multiplier: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        TOP_PRO_LMM / PRO_RATA_WITH_PRIORITY: per-order priority weight (default 1.0).
        """
    def set_queue_fifo_top_n(self, top_n: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        For pro_rata_with_fifo: first N orders at a level consume the trade FIFO; the rest is split pro-rata.
        """
    def set_queue_model(self, model: str, depth: typing.SupportsInt | typing.SupportsIndex = 1) -> None:
        """
        Configure queue simulation. model: none|tob|full|pro_rata|pro_rata_with_fifo|top_pro_lmm|pro_rata_with_priority
        """
    def set_queue_position_min_change_fraction(self, fraction: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Minimum fractional change in queue-ahead required to emit a queue-position event. 0.0 = every change, 1.0 = disable.
        """
    def set_rate_limit_policy(self, policy: typing.Any) -> None:
        ...
    def set_replace_ack_latency(self, latency_ns: typing.SupportsInt | typing.SupportsIndex, jitter_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def set_replace_ack_latency_distribution(self, dist: LatencyDistribution) -> None:
        ...
    def set_stp_group_membership(self, account_id: typing.SupportsInt | typing.SupportsIndex, group_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Opt an STP account into a group. group_id=0 removes the mapping.
        """
    def set_stp_mode(self, mode: str) -> None:
        """
        Self-trade prevention mode: none | cancel_newest | cancel_oldest | cancel_both | decrement.
        """
    def set_submit_ack_latency(self, latency_ns: typing.SupportsInt | typing.SupportsIndex, jitter_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def set_submit_ack_latency_distribution(self, dist: LatencyDistribution) -> None:
        ...
    def set_symbol_slippage(self, symbol: typing.SupportsInt | typing.SupportsIndex, model: str, ticks: typing.SupportsInt | typing.SupportsIndex = 0, tick_size: typing.SupportsFloat | typing.SupportsIndex = 0.0, bps: typing.SupportsFloat | typing.SupportsIndex = 0.0, impact_coeff: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        """
        Configure slippage for a specific symbol
        """
    def set_top_priority_share(self, share: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        TOP_PRO_LMM: fraction of each trade reserved for the queue-front order.
        """
    def set_venue_availability(self, availability: typing.Any) -> None:
        """
        Attach a VenueAvailability model. None to detach.
        """
    def stp_group_for(self, account_id: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def submit_bracket(self, bracket_id: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, entry_side: str, entry_type: str, entry_price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, tp_side: str, tp_type: str, tp_price: typing.SupportsFloat | typing.SupportsIndex, stop_side: str, stop_type: str, stop_trigger_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Submit a native bracket order (entry + take-profit + stop). The simulator manages the state machine: first child to fill cancels the other; cancelling cancels every live leg.
        """
    def submit_iceberg(self, order_id: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, total_quantity: typing.SupportsFloat | typing.SupportsIndex, visible_quantity: typing.SupportsFloat | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex = 1) -> None:
        """
        Submit a native iceberg order. Only visible_quantity is exposed; the hidden remainder refreshes automatically as the visible slice fills.
        """
    def submit_order(self, id: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, type: str = 'market', symbol: typing.SupportsInt | typing.SupportsIndex = 1, tif: str = 'gtc', reduce_only: bool = False, expires_at_ns: typing.SupportsInt | typing.SupportsIndex = 0, account_id: typing.SupportsInt | typing.SupportsIndex = 0, trigger: typing.SupportsFloat | typing.SupportsIndex = 0.0, trailing_offset: typing.SupportsFloat | typing.SupportsIndex = 0.0, trailing_bps: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        """
        Submit an order to the simulated exchange. type: one of lrvx.ORDER_TYPE_NAMES (market|limit|stop_market|stop_limit|tp_market|tp_limit|trailing_stop|iceberg); anything else raises ValueError. tif: gtc|ioc|fok|gtd|post_only. reduce_only: only reduce existing position. expires_at_ns: GTD deadline. account_id: optional STP account identifier (default 0). trigger: trigger price for stop_market / stop_limit / tp_market / tp_limit (falls back to `price` when unset, which is unambiguous for the market-style conditionals). trailing_offset / trailing_bps: fixed-price or bps offset for trailing_stop (set exactly one).
        """
    @property
    def fill_count(self) -> int:
        ...
class Skewness:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Slope:
    def __init__(self, length: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Stats:
    def __getitem__(self, arg0: str) -> typing.Any:
        ...
    def __repr__(self) -> str:
        ...
    def to_dict(self) -> dict:
        ...
    @property
    def avg_loss(self) -> float:
        ...
    @property
    def avg_win(self) -> float:
        ...
    @property
    def calmar_ratio(self) -> float:
        ...
    @property
    def final_capital(self) -> float:
        ...
    @property
    def gross_loss(self) -> float:
        ...
    @property
    def gross_profit(self) -> float:
        ...
    @property
    def initial_capital(self) -> float:
        ...
    @property
    def losing_trades(self) -> int:
        ...
    @property
    def max_drawdown(self) -> float:
        ...
    @property
    def max_drawdown_pct(self) -> float:
        ...
    @property
    def net_pnl(self) -> float:
        ...
    @property
    def profit_factor(self) -> float:
        ...
    @property
    def return_pct(self) -> float:
        ...
    @property
    def sharpe_ratio(self) -> float:
        ...
    @property
    def sortino_ratio(self) -> float:
        ...
    @property
    def total_fees(self) -> float:
        ...
    @property
    def total_pnl(self) -> float:
        ...
    @property
    def total_trades(self) -> int:
        ...
    @property
    def win_rate(self) -> float:
        ...
    @property
    def winning_trades(self) -> int:
        ...
class Stochastic:
    def __init__(self, k_period: typing.SupportsInt | typing.SupportsIndex = 14, d_period: typing.SupportsInt | typing.SupportsIndex = 3) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> dict:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def d(self) -> float | None:
        ...
    @property
    def k(self) -> float | None:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class StorageSink:
    def __init__(self) -> None:
        ...
    def store(self, signal: Signal) -> None:
        ...
class Strategy:
    def __init__(self, symbols: list) -> None:
        ...
    def bar_ring_capacity(self) -> int:
        ...
    def best_ask(self, symbol: str | None = None) -> float | None:
        ...
    def best_bid(self, symbol: str | None = None) -> float | None:
        ...
    def cancel_all_orders(self, symbol: str | None = None) -> None:
        ...
    def cancel_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def close_position(self, symbol: str | None = None) -> int:
        ...
    def ctx(self, symbol: typing.SupportsInt | typing.SupportsIndex | None = None) -> SymbolContext:
        ...
    def emit_cancel(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def emit_cancel_all(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def emit_close_position(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def emit_limit_buy(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_limit_buy_tif(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, tif: str = 'gtc') -> int:
        ...
    def emit_limit_sell(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_limit_sell_tif(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, tif: str = 'gtc') -> int:
        ...
    def emit_market_buy(self, symbol: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_market_sell(self, symbol: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_modify(self, order_id: typing.SupportsInt | typing.SupportsIndex, new_price: typing.SupportsFloat | typing.SupportsIndex, new_quantity: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def emit_provide_liquidity(self, pool: typing.SupportsInt | typing.SupportsIndex, price_lower: typing.SupportsFloat | typing.SupportsIndex, price_upper: typing.SupportsFloat | typing.SupportsIndex, liquidity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_stop_limit(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_stop_market(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_take_profit_limit(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_take_profit_market(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_trailing_stop(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, offset: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_trailing_stop_percent(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, callback_bps: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_withdraw_liquidity(self, pool: typing.SupportsInt | typing.SupportsIndex, liquidity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def get_order_status(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def last_closed_bar(self, symbol_id: typing.SupportsInt | typing.SupportsIndex, bar_type: typing.SupportsInt | typing.SupportsIndex = 0, param: typing.SupportsInt | typing.SupportsIndex = 0) -> dict | None:
        """
        Return the last closed bar for (symbol, bar_type, param) or None if no bar of that timeframe has been emitted yet. bar_type: 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi, 6=BpsRange. param: time bar interval in nanoseconds, or the tick / volume / range threshold the aggregator was configured with.
        """
    def last_n_closed_bars(self, symbol_id: typing.SupportsInt | typing.SupportsIndex, bar_type: typing.SupportsInt | typing.SupportsIndex, param: typing.SupportsInt | typing.SupportsIndex, n: typing.SupportsInt | typing.SupportsIndex) -> list:
        """
        Return the most recent up to n closed bars for the given (symbol, timeframe), oldest first. Returns an empty list until the aggregator has emitted at least one bar.
        """
    def last_price(self, symbol: str | None = None) -> float:
        ...
    def limit_buy(self, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None, tif: str = 'gtc') -> int:
        ...
    def limit_sell(self, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None, tif: str = 'gtc') -> int:
        ...
    def market_buy(self, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def market_sell(self, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def mid_price(self, symbol: str | None = None) -> float | None:
        ...
    def modify_order(self, order_id: typing.SupportsInt | typing.SupportsIndex, new_price: typing.SupportsFloat | typing.SupportsIndex, new_qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def on_bar(self, ctx: SymbolContext, bar: BarData) -> None:
        ...
    def on_book_update(self, ctx: SymbolContext) -> None:
        ...
    def on_fill(self, ctx: SymbolContext, event: OrderEventData) -> None:
        ...
    def on_market_position_change(self, ctx: SymbolContext, event: OrderEventData) -> None:
        ...
    def on_order_update(self, ctx: SymbolContext, event: OrderEventData) -> None:
        ...
    def on_queue_position_change(self, ctx: SymbolContext, event: OrderEventData) -> None:
        ...
    def on_start(self) -> None:
        ...
    def on_stop(self) -> None:
        ...
    def on_trade(self, ctx: SymbolContext, trade: TradeData) -> None:
        ...
    def order_status(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def pos(self, symbol: str | None = None) -> float:
        ...
    def position(self, symbol: typing.SupportsInt | typing.SupportsIndex | None = None) -> float:
        ...
    def set_bar_ring_capacity(self, n: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def stop_limit(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def stop_market(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def take_profit_limit(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def take_profit_market(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def trailing_stop(self, side: str, offset: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def trailing_stop_percent(self, side: str, callback_bps: typing.SupportsInt | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    @property
    def primary_symbol_name(self) -> str:
        ...
    @property
    def symbol_names(self) -> list[str]:
        ...
    @property
    def symbols(self) -> list[int]:
        ...
class Symbol:
    def __eq__(self, arg0: object) -> bool:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __int__(self) -> int:
        ...
    def __repr__(self) -> str:
        ...
    def __str__(self) -> str:
        ...
    @property
    def exchange(self) -> str:
        ...
    @property
    def id(self) -> int:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def tick_size(self) -> float:
        ...
class SymbolContext:
    symbol: str
    def __init__(self) -> None:
        ...
    def book_spread(self) -> float | None:
        ...
    def is_flat(self) -> bool:
        ...
    def is_long(self) -> bool:
        ...
    def is_short(self) -> bool:
        ...
    @property
    def best_ask(self) -> float | None:
        ...
    @best_ask.setter
    def best_ask(self, arg0: typing.SupportsFloat | typing.SupportsIndex | None) -> None:
        ...
    @property
    def best_bid(self) -> float | None:
        ...
    @best_bid.setter
    def best_bid(self, arg0: typing.SupportsFloat | typing.SupportsIndex | None) -> None:
        ...
    @property
    def last_trade_price(self) -> float:
        ...
    @last_trade_price.setter
    def last_trade_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def mid_price(self) -> float | None:
        ...
    @mid_price.setter
    def mid_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex | None) -> None:
        ...
    @property
    def position(self) -> float:
        ...
    @position.setter
    def position(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol_id(self) -> int:
        ...
    @symbol_id.setter
    def symbol_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def unrealized_pnl(self) -> float:
        ...
    @unrealized_pnl.setter
    def unrealized_pnl(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class SymbolRegistry:
    def __init__(self) -> None:
        ...
    def add_symbol(self, exchange: str, symbol: str, tick_size: typing.SupportsFloat | typing.SupportsIndex = 0.01) -> Symbol:
        ...
    def symbol_count(self) -> int:
        ...
class TEMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class TapeRef:
    content_hash: str
    path: str
    @typing.overload
    def __init__(self) -> None:
        ...
    @typing.overload
    def __init__(self, path: str, content_hash: str = '', first_event_ns: typing.SupportsInt | typing.SupportsIndex = 0, last_event_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    @property
    def first_event_ns(self) -> int:
        ...
    @first_event_ns.setter
    def first_event_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def last_event_ns(self) -> int:
        ...
    @last_event_ns.setter
    def last_event_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class TraceReader:
    def __init__(self, path: str) -> None:
        ...
    def read_all_fills(self) -> list:
        ...
    def read_all_order_events(self) -> list:
        ...
    def read_all_signals(self) -> list:
        ...
    @property
    def run_ended_ns(self) -> int:
        ...
    @property
    def run_started_ns(self) -> int:
        ...
    @property
    def strategy_hash(self) -> str:
        ...
    @property
    def strategy_id(self) -> str:
        ...
    @property
    def tape_refs(self) -> list:
        ...
class TraceRecorder:
    def __init__(self, path: str, strategy_id: str = '', strategy_hash: str = '', run_started_ns: typing.SupportsInt | typing.SupportsIndex = 0, tape_refs: collections.abc.Sequence[TapeRef] = []) -> None:
        ...
    def close(self) -> None:
        ...
    def set_run_ended_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def write_fill(self, run_ts_ns: typing.SupportsInt | typing.SupportsIndex, feed_ts_ns: typing.SupportsInt | typing.SupportsIndex = 0, order_id: typing.SupportsInt | typing.SupportsIndex = 0, fill_id: typing.SupportsInt | typing.SupportsIndex = 0, price_raw: typing.SupportsInt | typing.SupportsIndex = 0, qty_raw: typing.SupportsInt | typing.SupportsIndex = 0, fee_raw: typing.SupportsInt | typing.SupportsIndex = 0, symbol_id: typing.SupportsInt | typing.SupportsIndex = 0, side: typing.SupportsInt | typing.SupportsIndex = 0, liquidity: FillLiquidity = ...) -> None:
        ...
    def write_order_event(self, run_ts_ns: typing.SupportsInt | typing.SupportsIndex, feed_ts_ns: typing.SupportsInt | typing.SupportsIndex = 0, order_id: typing.SupportsInt | typing.SupportsIndex = 0, parent_signal_id: typing.SupportsInt | typing.SupportsIndex = 0, price_raw: typing.SupportsInt | typing.SupportsIndex = 0, qty_raw: typing.SupportsInt | typing.SupportsIndex = 0, symbol_id: typing.SupportsInt | typing.SupportsIndex = 0, event_kind: OrderEventKind = ..., side: typing.SupportsInt | typing.SupportsIndex = 0, order_type: typing.SupportsInt | typing.SupportsIndex = 0, flags: typing.SupportsInt | typing.SupportsIndex = 0, reason: str = '') -> None:
        ...
    def write_signal(self, run_ts_ns: typing.SupportsInt | typing.SupportsIndex, feed_ts_ns: typing.SupportsInt | typing.SupportsIndex = 0, signal_id: typing.SupportsInt | typing.SupportsIndex = 0, flags: typing.SupportsInt | typing.SupportsIndex = 0, strength_raw: typing.SupportsInt | typing.SupportsIndex = 0, name: str = '', symbol_ids: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = [], payload: bytes = b'') -> None:
        ...
class TradeData:
    is_buy: bool
    side: str
    symbol_name: str
    def __init__(self) -> None:
        ...
    @property
    def exchange_ts_ns(self) -> int:
        ...
    @exchange_ts_ns.setter
    def exchange_ts_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def price(self) -> float:
        ...
    @price.setter
    def price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def quantity(self) -> float:
        ...
    @quantity.setter
    def quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def timestamp_ns(self) -> int:
        ...
    @timestamp_ns.setter
    def timestamp_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class VenueAvailability:
    def __init__(self) -> None:
        ...
    def auto_random_outages(self, per_day: typing.SupportsFloat | typing.SupportsIndex, mean_duration_ns: typing.SupportsInt | typing.SupportsIndex, on_open_orders: str = 'cancel_all', seed: typing.SupportsInt | typing.SupportsIndex = 12648430) -> VenueAvailability:
        ...
    def book_updates_allowed(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    def cancels_allowed(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    def consume_wrong_side_recovery_bps(self) -> float:
        ...
    def is_up(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    def latency_multiplier(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> float:
        ...
    def schedule_outage(self, start_ns: typing.SupportsInt | typing.SupportsIndex, duration_ns: typing.SupportsInt | typing.SupportsIndex, on_open_orders: str = 'cancel_all', gtc_ttl_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> VenueAvailability:
        ...
    def schedule_outage_ex(self, start_ns: typing.SupportsInt | typing.SupportsIndex, duration_ns: typing.SupportsInt | typing.SupportsIndex, outage_type: str = 'total', on_open_orders: str = 'cancel_all', gtc_ttl_ns: typing.SupportsInt | typing.SupportsIndex = 0, degradation_latency_multiplier: typing.SupportsFloat | typing.SupportsIndex = 1.0, wrong_side_recovery_bps: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> VenueAvailability:
        ...
    def submits_allowed(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    def trades_allowed(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
class VenueExecutor:
    """
    Executor handed out by VenueStack.executor(). Same simulated
    engine as standalone SimulatedExecutor, but the instance is
    owned by the venue stack and shares its account, fees, funding,
    liquidation, and rate-limit policy. Use this when you want to
    drive the venue-realistic stack from Python — RL envs, paper
    brokers, custom Strategy harnesses.
    """
    def cancel_all(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def cancel_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def clear_fills(self) -> None:
        """
        Discard every fill recorded so far. `fills_list()` and `fill_count` read the same accumulating buffer as the plain PySimulatedExecutor wrapper; nothing resets it between runs on its own, so a caller that reuses one executor across repeated episodes (an RL training loop, a walk-forward fold) must call this explicitly or fills keep growing forever.
        """
    def clear_rate_limit_policy(self) -> None:
        ...
    def fills_list(self) -> list:
        """
        Every fill recorded on this executor as a list of dicts.
        """
    def on_bar(self, symbol: typing.SupportsInt | typing.SupportsIndex, close_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Feed a bar close for order matching.
        """
    def on_trade(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        """
        Feed a trade tick for order matching.
        """
    def on_trade_qty(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        """
        Feed a trade tick with quantity (enables queue-fill simulation).
        """
    def set_rate_limit_policy(self, policy: RateLimitPolicy) -> None:
        ...
    def set_venue_availability(self, venue_availability: VenueAvailability) -> None:
        ...
    def submit_order(self, id: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, type: str = 'market', symbol: typing.SupportsInt | typing.SupportsIndex = 1, tif: str = 'gtc', reduce_only: bool = False, expires_at_ns: typing.SupportsInt | typing.SupportsIndex = 0, account_id: typing.SupportsInt | typing.SupportsIndex = 0, trigger: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        """
        Submit an order. side: buy|sell. type: one of lrvx.ORDER_TYPE_NAMES (market|limit|stop_market|stop_limit|tp_market|tp_limit|trailing_stop|iceberg); anything else raises ValueError. tif: gtc|ioc|fok|gtd|post_only. trigger: arming price for the conditional types, defaulting to `price`.
        """
    @property
    def fill_count(self) -> int:
        ...
class VenueStack:
    """
    Single-call venue-realistic backtest stack.
    
    Use one of the factory methods to obtain a fully-wired stack (executor + account + liquidation + fees + funding + rate limits + venue availability). The bare SimulatedExecutor() constructor remains for unit tests of the executor itself; for backtests of real strategies always go through a venue factory below.
    """
    @staticmethod
    def binance_um_futures(account_id: typing.SupportsInt | typing.SupportsIndex, equity: typing.SupportsFloat | typing.SupportsIndex) -> VenueStack:
        ...
    @staticmethod
    def bybit_linear(account_id: typing.SupportsInt | typing.SupportsIndex, equity: typing.SupportsFloat | typing.SupportsIndex) -> VenueStack:
        ...
    @staticmethod
    def deribit(account_id: typing.SupportsInt | typing.SupportsIndex, equity: typing.SupportsFloat | typing.SupportsIndex) -> VenueStack:
        ...
    @staticmethod
    def from_venue(name: str, account_id: typing.SupportsInt | typing.SupportsIndex, equity: typing.SupportsFloat | typing.SupportsIndex) -> VenueStack:
        """
        Dispatcher by venue name. Accepts binance_um_futures, bybit_linear, okx_swap, deribit (case-insensitive). Returns an empty stack on unknown name; check via venue_name().
        """
    @staticmethod
    def okx_swap(account_id: typing.SupportsInt | typing.SupportsIndex, equity: typing.SupportsFloat | typing.SupportsIndex) -> VenueStack:
        ...
    def account(self) -> Account:
        ...
    def clock(self) -> SimulatedClock:
        ...
    def executor(self) -> VenueExecutor:
        ...
    def fees(self) -> FeeSchedule:
        ...
    def funding(self) -> FundingSchedule:
        ...
    def liquidation(self) -> LiquidationEngine:
        ...
    def venue(self) -> VenueAvailability:
        ...
    def venue_name(self) -> str:
        ...
class VolSurface:
    """
    SVI implied-volatility surface: a term structure of calibrated slices that interpolates vol in total-variance space. Mark a backtest to this instead of a flat vol.
    """
    def __init__(self) -> None:
        ...
    def add_slice(self, t: typing.SupportsFloat | typing.SupportsIndex, a: typing.SupportsFloat | typing.SupportsIndex, b: typing.SupportsFloat | typing.SupportsIndex, rho: typing.SupportsFloat | typing.SupportsIndex, m: typing.SupportsFloat | typing.SupportsIndex, sigma: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Add a calibrated SVI slice at expiry t (years).
        """
    def implied_vol(self, log_moneyness: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex) -> float:
        """
        Black-Scholes implied vol at (log-moneyness, expiry).
        """
    def is_calendar_free(self, k_lo: typing.SupportsFloat | typing.SupportsIndex = -1.5, k_hi: typing.SupportsFloat | typing.SupportsIndex = 1.5, samples: typing.SupportsInt | typing.SupportsIndex = 50) -> bool:
        """
        True when total variance is non-decreasing in time (no calendar arbitrage).
        """
    def slice_count(self) -> int:
        ...
    def total_variance(self, log_moneyness: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex) -> float:
        """
        Total implied variance at (log-moneyness, expiry).
        """
class VolumeBinAggregator(_AggregatorHandle):
    """
    Time-bucketed trade quantity sum. Same bucketing structure as BinCountAggregator but sums trade.qty_raw per cell instead of counting. Books are ignored (no scalar qty per book event).
    """
    def __init__(self, bucket_ns: typing.SupportsInt | typing.SupportsIndex, by_side: bool = False, by_symbol: bool = False, event_filter: AggregatorEventFilter = ..., symbol_filter: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> None:
        ...
    def result(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Structured numpy: (bucket_ts_ns i8, symbol_id u4, side u1, qty_raw i8). qty_raw is fixed-point — divide by Quantity::SCALE to get floats.
        """
class VolumeProfile:
    """
    Volume Profile aggregator. Tracks volume distribution across price levels.
    """
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def add_trade(self, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        ...
    def add_trades(self, prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> None:
        ...
    def clear(self) -> None:
        ...
    def levels(self) -> list:
        ...
    def num_levels(self) -> int:
        ...
    def poc(self) -> float:
        ...
    def total_delta(self) -> float:
        ...
    def total_volume(self) -> float:
        ...
    def value_area_high(self) -> float:
        ...
    def value_area_low(self) -> float:
        ...
    def volume_at(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
class WalkForwardRunner:
    def __init__(self, registry: typing.Any, fee_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0004, initial_capital: typing.SupportsFloat | typing.SupportsIndex = 10000.0, mode: str = 'anchored', train_size: typing.SupportsInt | typing.SupportsIndex = 0, test_size: typing.SupportsInt | typing.SupportsIndex = 0, step: typing.SupportsInt | typing.SupportsIndex = 0, min_train_size: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def run_bars(self, start_time_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], end_time_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], open: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], symbol: str, bar_type: typing.SupportsInt | typing.SupportsIndex = 0, bar_type_param: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
        """
        Walk-forward over a sequence of full OHLCV bars. Each fold dispatches BarEvents through Strategy.on_bar with open / high / low / close / volume preserved. Bars must be sorted by end_time_ns. bar_type: 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi.
        """
    def run_csv(self, path: str, symbol: str) -> list:
        """
        Walk-forward over an OHLCV CSV. Bars are dispatched to the strategy as synthetic trade events at close price — useful for on_trade strategies. For on_bar strategies that need open/high/low/volume, use run_bars().
        """
    def set_strategy_factory(self, factory: typing.Any) -> None:
        """
        Callable[[int], Strategy] — receives fold_index, returns a fresh Strategy instance per fold (called twice per fold: train + test).
        """
class _AggregatorHandle:
    pass
class _ExecOrderType:
    """
    Members:
    
      Market
    
      Limit
    """
    Limit: typing.ClassVar[_ExecOrderType]  # value = <_ExecOrderType.Limit: 1>
    Market: typing.ClassVar[_ExecOrderType]  # value = <_ExecOrderType.Market: 0>
    __members__: typing.ClassVar[dict[str, _ExecOrderType]]  # value = {'Market': <_ExecOrderType.Market: 0>, 'Limit': <_ExecOrderType.Limit: 1>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class _ExecSide:
    """
    Members:
    
      Buy
    
      Sell
    """
    Buy: typing.ClassVar[_ExecSide]  # value = <_ExecSide.Buy: 0>
    Sell: typing.ClassVar[_ExecSide]  # value = <_ExecSide.Sell: 1>
    __members__: typing.ClassVar[dict[str, _ExecSide]]  # value = {'Buy': <_ExecSide.Buy: 0>, 'Sell': <_ExecSide.Sell: 1>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class _ExecutionAlgoBase:
    def clear_pending(self) -> None:
        ...
    def is_done(self) -> bool:
        ...
    def observe_volume(self, qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def pending(self) -> list:
        """
        Return the pending child-order list as a Python list of dicts.
        """
    def report_fill(self, qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def step(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def filled_qty(self) -> float:
        ...
    @property
    def remaining_qty(self) -> float:
        ...
    @property
    def submitted_qty(self) -> float:
        ...
    @property
    def target_qty(self) -> float:
        ...
class _IcebergExecutorNative(_ExecutionAlgoBase):
    def __init__(self, target_qty: typing.SupportsFloat | typing.SupportsIndex, side: _ExecSide, visible_qty: typing.SupportsFloat | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex = 0, type: _ExecOrderType = ..., limit_price: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        ...
class _POVExecutorNative(_ExecutionAlgoBase):
    def __init__(self, target_qty: typing.SupportsFloat | typing.SupportsIndex, side: _ExecSide, participation_rate: typing.SupportsFloat | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex = 0, type: _ExecOrderType = ..., limit_price: typing.SupportsFloat | typing.SupportsIndex = 0.0, min_slice_qty: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        ...
class _PortfolioRiskAggregator:
    """
    C++-backed aggregator. Public Python users go through lrvx.portfolio_risk.PortfolioRiskAggregator which preserves the existing dataclass surface.
    """
    def __init__(self, rules: _PortfolioRiskRules = ..., initial_equity: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        ...
    def check_order(self, strategy: str, notional: typing.SupportsFloat | typing.SupportsIndex, side: str) -> typing.Any:
        ...
    def remove(self, name: str) -> None:
        ...
    def reset_kill_switch(self) -> None:
        ...
    def snapshot(self) -> dict:
        ...
    def update(self, name: str, realized_pnl: typing.SupportsFloat | typing.SupportsIndex = 0.0, unrealized_pnl: typing.SupportsFloat | typing.SupportsIndex = 0.0, fees: typing.SupportsFloat | typing.SupportsIndex = 0.0, gross_exposure: typing.SupportsFloat | typing.SupportsIndex = 0.0, net_exposure: typing.SupportsFloat | typing.SupportsIndex = 0.0, trade_count: typing.SupportsInt | typing.SupportsIndex = 0, field_mask: typing.SupportsInt | typing.SupportsIndex = 63) -> None:
        ...
class _PortfolioRiskRules:
    """
    Internal C++-backed risk rules struct.
    """
    def __init__(self) -> None:
        ...
    @property
    def max_concentration_pct(self) -> float | None:
        ...
    @max_concentration_pct.setter
    def max_concentration_pct(self, arg0: typing.SupportsFloat | typing.SupportsIndex | None) -> None:
        ...
    @property
    def max_daily_loss(self) -> float | None:
        ...
    @max_daily_loss.setter
    def max_daily_loss(self, arg0: typing.SupportsFloat | typing.SupportsIndex | None) -> None:
        ...
    @property
    def max_drawdown_pct(self) -> float | None:
        ...
    @max_drawdown_pct.setter
    def max_drawdown_pct(self, arg0: typing.SupportsFloat | typing.SupportsIndex | None) -> None:
        ...
    @property
    def max_gross_exposure(self) -> float | None:
        ...
    @max_gross_exposure.setter
    def max_gross_exposure(self, arg0: typing.SupportsFloat | typing.SupportsIndex | None) -> None:
        ...
class _TWAPExecutorNative(_ExecutionAlgoBase):
    def __init__(self, target_qty: typing.SupportsFloat | typing.SupportsIndex, side: _ExecSide, symbol: typing.SupportsInt | typing.SupportsIndex = 0, type: _ExecOrderType = ..., limit_price: typing.SupportsFloat | typing.SupportsIndex = 0.0, duration_ns: typing.SupportsInt | typing.SupportsIndex = 0, slice_count: typing.SupportsInt | typing.SupportsIndex = 0, start_time_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
class _VWAPExecutorNative(_ExecutionAlgoBase):
    def __init__(self, target_qty: typing.SupportsFloat | typing.SupportsIndex, side: _ExecSide, volume_curve: collections.abc.Sequence[tuple[typing.SupportsInt | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]], symbol: typing.SupportsInt | typing.SupportsIndex = 0, type: _ExecOrderType = ..., limit_price: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        ...
def _tape_diff_native(left: str, right: str, max_mismatches: typing.SupportsInt | typing.SupportsIndex = 16, field_tolerance_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> typing.Any:
    """
    Internal C++-backed tape diff. Returns a dict the public wrapper in lrvx.tape converts to a TapeDiff dataclass.
    """
def adf(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], max_lag: typing.SupportsInt | typing.SupportsIndex = 4, regression: str = 'c') -> dict:
    ...
def adx(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 14) -> dict:
    ...
def aggregate_heikin_ashi_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], interval_seconds: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into Heikin-Ashi bars
    """
def aggregate_range_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], range_size: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into range bars
    """
def aggregate_renko_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], brick_size: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into renko bars
    """
def aggregate_tick_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], tick_count: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into tick bars
    """
def aggregate_time_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], interval_seconds: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into time bars
    """
def aggregate_volume_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], volume_threshold: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into volume bars
    """
def atr(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def autocorrelation(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], window: typing.SupportsInt | typing.SupportsIndex, lag: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def bar_returns(signal_long: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], signal_short: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], log_returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def baw_price(option_type: OptionType, spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, vol: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, carry: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> float:
    """
    Barone-Adesi-Whaley American price: European value plus a closed-form early-exercise premium. Cheaper than a fine tree, so preferred for greeks via finite differences. An American call with carry>=rate collapses to the European price.
    """
def binomial_price(option_type: OptionType, spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, vol: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, carry: typing.SupportsFloat | typing.SupportsIndex = 0.0, steps: typing.SupportsInt | typing.SupportsIndex = 200, american: bool = True) -> float:
    """
    Cox-Ross-Rubinstein binomial price. american=True checks early exercise at every node (American premium); american=False is a European lattice that converges to bs_price as steps grows.
    """
def bollinger(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 20, stddev: typing.SupportsFloat | typing.SupportsIndex = 2.0) -> dict:
    ...
def bootstrap_ci(data: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], confidence: typing.SupportsFloat | typing.SupportsIndex = 0.95, num_samples: typing.SupportsInt | typing.SupportsIndex = 10000) -> tuple[float, float, float]:
    """
    Bootstrap confidence interval, returns (lower, median, upper)
    """
def bs_price(option_type: OptionType, spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, vol: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, carry: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> float:
    """
    Generalized Black-Scholes-Merton price. t in years; rate=discount r; carry=cost-of-carry b (0 for crypto/Black-76). Returns discounted intrinsic for expired or zero-vol inputs.
    """
def bs_price_discrete_dividends(option_type: OptionType, spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, vol: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex, dividends: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]]) -> float:
    """
    European price on a stock paying discrete cash dividends (escrowed model). dividends is a list of (t_years, amount) paid before expiry; their present value is subtracted from spot. A dividend lowers a call and lifts a put.
    """
def bs_vega(spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, vol: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, carry: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> float:
    """
    Black-Scholes vega (call/put identical).
    """
def build_surface_as_of(quotes: collections.abc.Sequence[tuple[typing.SupportsInt | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]], asof_ns: typing.SupportsInt | typing.SupportsIndex) -> VolSurface:
    """
    Build a point-in-time VolSurface from (ts_ns, t, log_moneyness, iv) quotes, using ONLY those stamped on or before asof_ns — the no-lookahead guarantee for honest backtests. Expiries with < 5 quotes are skipped.
    """
def calibrate_svi(log_moneyness: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], total_variance: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex]) -> dict:
    """
    Calibrate a raw-SVI slice to observed (log-moneyness, total-variance) points by least squares. Needs >= 5 points. Returns dict(a, b, rho, m, sigma). Total variance is iv**2 * t.
    """
def capi_abi_version() -> int:
    """
    C ABI version the loaded lrvx library reports. Equal to
    CAPI_ABI_VERSION, which the extension was compiled against --
    the import fails outright when they disagree.
    """
def cci(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 20) -> numpy.typing.NDArray[numpy.float64]:
    ...
def chop(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 14) -> numpy.typing.NDArray[numpy.float64]:
    ...
def correlation(x: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], y: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> float:
    """
    Pearson correlation coefficient
    """
def cost_of_carry(rate: typing.SupportsFloat | typing.SupportsIndex, dividend_yield: typing.SupportsFloat | typing.SupportsIndex = 0.0, borrow_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> float:
    """
    Cost-of-carry b = rate - dividend_yield - borrow_rate. Crypto/Black-76 pass all zero; equity nets the financing pieces; FX passes the foreign rate as dividend_yield.
    """
def cvd(open: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def deflated_sharpe(returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], trial_sharpes: typing.Annotated[numpy.typing.ArrayLike, numpy.float64] | None = None) -> dict:
    """
    Deflated Sharpe ratio. `returns` is the selected strategy's per-period return series; `trial_sharpes` holds the per-period Sharpe ratio of every trial the selection ran over (None or fewer than two means no selection, so the expected maximum is 0 and dsr is the probabilistic Sharpe ratio against zero). Returns sharpe, skewness, kurtosis (3 for normal returns), expected_max_sharpe and dsr, the probability that the true Sharpe ratio exceeds the expected maximum of that many unskilled trials. NaN with fewer than two periods or a zero return variance.
    """
def dema(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def ema(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def export_data(input_path: str, output_path: str, format: str = 'csv', from_ns: typing.Any = None, to_ns: typing.Any = None, symbols: typing.Any = None) -> dict:
    ...
def extract_symbols(input_path: str, output_path: str, symbols: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex]) -> int:
    ...
def extract_time_range(input_path: str, output_path: str, from_ns: typing.SupportsInt | typing.SupportsIndex, to_ns: typing.SupportsInt | typing.SupportsIndex) -> int:
    ...
def forward_price(spot: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, carry: typing.SupportsFloat | typing.SupportsIndex) -> float:
    """
    Forward price spot * exp(carry * t).
    """
def greeks(option_type: OptionType, spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, vol: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, carry: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> dict:
    """
    First-order greeks dict(delta, gamma, vega, theta, rho). theta is per year (divide by 365 for per-day); rho is the b-fixed partial dV/dr. carry=b (0 for crypto).
    """
def grid_neighborhood(values: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], radius: typing.SupportsInt | typing.SupportsIndex = 1) -> dict:
    """
    The neighbourhood of every point of a parameter grid. `values` is an n-dimensional array of one metric per grid point; a cell's neighbours are the cells whose index differs by at most `radius` on every axis, the cell itself excluded, NaN values skipped. Returns neighbor_mean (the mean of the neighbours' values, NaN when there are none) and neighbor_count, both shaped like `values`.
    """
def hurst_dfa(returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], scales: typing.Any = None) -> float:
    """
    Hurst exponent via Detrended Fluctuation Analysis on a series of returns (NOT prices). H ≈ 0.5 random, H > 0.5 persistent / trending, H < 0.5 anti-persistent / mean-reverting. If `scales` is None, uses a log-spaced default grid from 4 to N/4. Returns NaN if input is too short (< 32) or degenerate.
    """
def i256_roundtrip(value: int) -> int:
    ...
def implied_percentile_in_cone(realized_samples: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], implied_vol: typing.SupportsFloat | typing.SupportsIndex) -> float:
    """
    Fraction of realized-vol samples at or below implied_vol (0..1). High = options rich vs realized history; low = cheap. NaN on empty samples.
    """
def implied_vol(option_type: OptionType, price: typing.SupportsFloat | typing.SupportsIndex, spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, carry: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> dict:
    """
    Implied volatility from an option price via Newton-Raphson with bracketed bisection fallback. Returns dict(vol, converged, iterations); vol is NaN and converged False when the price violates no-arbitrage bounds.
    """
def inspect(data_dir: str) -> dict:
    """
    Inspect a data directory and return summary statistics
    """
def kama(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 10) -> numpy.typing.NDArray[numpy.float64]:
    ...
def kurtosis(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def list_indicators() -> list:
    """
    Return the list of indicator names registered in this build (both batch compute() and streaming update()/value on each).
    """
def macd(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], fast: typing.SupportsInt | typing.SupportsIndex = 12, slow: typing.SupportsInt | typing.SupportsIndex = 26, signal: typing.SupportsInt | typing.SupportsIndex = 9) -> dict:
    ...
def merge(input_paths: collections.abc.Sequence[str], output_dir: str, output_name: str = 'merged', compression: str = 'none', sort: bool = True) -> dict:
    ...
def merge_dir(input_dir: str, output_dir: str) -> dict:
    ...
def obv(close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def order_type_code(name: str) -> int:
    """
    Wire code of a canonical order-type name (lrvx::OrderType space).
    Raises ValueError naming the accepted set for anything else.
    """
def order_type_name(code: typing.SupportsInt | typing.SupportsIndex) -> str:
    """
    Canonical name of an order-type wire code (lrvx::OrderType space).
    Raises ValueError for a code outside ORDER_TYPE_NAMES.
    """
def parkinson_vol(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def pbo_cscv(returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], num_blocks: typing.SupportsInt | typing.SupportsIndex = 16) -> dict:
    """
    Probability of backtest overfitting by combinatorially symmetric cross-validation. `returns` is a (trials, periods) array of per-period returns, one row per parameter combination; `num_blocks` is the even number of period blocks the C(S, S/2) splits are drawn from. Returns pbo (the fraction of splits whose in-sample best ranks below the out-of-sample median; NaN when the inputs cannot be split), splits, mean_oos_rank and mean_logit. Deterministic.
    """
def permutation_test(group1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], group2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], num_permutations: typing.SupportsInt | typing.SupportsIndex = 10000) -> float:
    """
    Two-sample permutation test, returns p-value
    """
def prices_to_double(raw: typing.Annotated[numpy.typing.ArrayLike, numpy.int64]) -> numpy.typing.NDArray[numpy.float64]:
    """
    Convert raw int64 price array to float64 array (divides by PRICE_SCALE).
    """
def profit_factor(returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> float:
    ...
def quantities_to_double(raw: typing.Annotated[numpy.typing.ArrayLike, numpy.int64]) -> numpy.typing.NDArray[numpy.float64]:
    """
    Convert raw int64 quantity array to float64 array (divides by QUANTITY_SCALE).
    """
def realized_vol(returns: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], periods_per_year: typing.SupportsFloat | typing.SupportsIndex) -> float:
    """
    Annualized realized vol from log returns: sample stdev * sqrt(periods_per_year).
    """
def recompress(input_path: str, output_path: str, compression: str = 'lz4') -> bool:
    ...
def rma(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def rogers_satchell_vol(open: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def rolling_correlation(x: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], y: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def rolling_hurst(prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], window: typing.SupportsInt | typing.SupportsIndex = 1080) -> numpy.typing.NDArray[numpy.float64]:
    """
    Rolling DFA-Hurst on price levels. For each bar i, computes Hurst of log-returns over the previous `window` returns. Output[i] = NaN for i ≤ window (insufficient history). Output length matches input.
    """
def rolling_zscore(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def rsi(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def second_order_greeks(option_type: OptionType, spot: typing.SupportsFloat | typing.SupportsIndex, strike: typing.SupportsFloat | typing.SupportsIndex, t: typing.SupportsFloat | typing.SupportsIndex, vol: typing.SupportsFloat | typing.SupportsIndex, rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, carry: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> dict:
    """
    Second-order greeks dict(vanna, volga, charm) for vol traders. vanna = d(delta)/d(vol), volga = d(vega)/d(vol), charm = d(delta)/d(time) per year.
    """
def set_log_callback(callback: typing.Any) -> None:
    """
    Install a Python callable as the global log sink. Pass None to detach. Callable receives (level: int, msg: str); level: 0=info, 1=warn, 2=error. The binding detaches it automatically at interpreter shutdown.
    """
def shannon_entropy(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex, bins: typing.SupportsInt | typing.SupportsIndex = 10) -> numpy.typing.NDArray[numpy.float64]:
    ...
def skewness(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def slope(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], length: typing.SupportsInt | typing.SupportsIndex = 1) -> numpy.typing.NDArray[numpy.float64]:
    ...
def sma(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def split(input_path: str, output_dir: str, mode: str = 'time', time_interval_ns: typing.SupportsInt | typing.SupportsIndex = 3600000000000, events_per_file: typing.SupportsInt | typing.SupportsIndex = 1000000) -> dict:
    ...
def stochastic(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], k_period: typing.SupportsInt | typing.SupportsIndex = 14, d_period: typing.SupportsInt | typing.SupportsIndex = 3) -> dict:
    ...
def tema(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def trade_pnl(signal_long: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], signal_short: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], log_returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def u256_from_hex(hex: str) -> int:
    ...
def u256_roundtrip(value: int) -> int:
    ...
def validate(segment_path: str, verify_crc: bool = True, verify_timestamps: bool = True) -> dict:
    ...
def validate_dataset(data_dir: str) -> dict:
    ...
def vol_cone(prices: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], horizons: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex], periods_per_year: typing.SupportsFloat | typing.SupportsIndex) -> list:
    """
    Realized-vol cone from a price series: for each horizon (in return periods) slide a window, annualize realized vol, and report a dict(horizon, min, p25, p50, p75, max, samples). periods_per_year is 252 (equity) or 365 (crypto).
    """
def volumes_to_double(raw: typing.Annotated[numpy.typing.ArrayLike, numpy.int64]) -> numpy.typing.NDArray[numpy.float64]:
    """
    Convert raw int64 volume array to float64 array (divides by VOLUME_SCALE).
    """
def vwap(close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], window: typing.SupportsInt | typing.SupportsIndex = 96) -> numpy.typing.NDArray[numpy.float64]:
    ...
def whites_reality_check(returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], num_bootstrap: typing.SupportsInt | typing.SupportsIndex = 10000, avg_block_size: typing.SupportsFloat | typing.SupportsIndex = 0.0, seed: typing.SupportsInt | typing.SupportsIndex = 42) -> dict:
    """
    White's reality check (Stationary Bootstrap). Tests whether the
    best-performing strategy among K candidates is significantly better
    than zero after multiple-comparison correction. The caller passes
    EXCESS returns (relative to a benchmark) shaped (K, T). Returns
    {p_value, best_stat, best_index}.
    """
def win_rate(trade_pnls: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> float:
    ...
ALL_OR_NOTHING: OrderGroupPolicy  # value = <OrderGroupPolicy.ALL_OR_NOTHING: 1>
BEST_EFFORT: OrderGroupPolicy  # value = <OrderGroupPolicy.BEST_EFFORT: 0>
Binance: AdlRanking  # value = <AdlRanking.Binance: 1>
BookAnchored: MarkImpactModel  # value = <MarkImpactModel.BookAnchored: 1>
BookOnly: MarkImpactModel  # value = <MarkImpactModel.BookOnly: 2>
Bybit: AdlRanking  # value = <AdlRanking.Bybit: 2>
CANCELLED: OrderGroupState  # value = <OrderGroupState.CANCELLED: 4>
CAPI_ABI_VERSION: int = 5
Cross: MarginMode  # value = <MarginMode.Cross: 0>
FAILED: OrderGroupState  # value = <OrderGroupState.FAILED: 6>
FILLED: OrderGroupState  # value = <OrderGroupState.FILLED: 3>
FIRE_ON_ANY: FeedClockPolicy  # value = <FeedClockPolicy.FIRE_ON_ANY: 1>
Isolated: MarginMode  # value = <MarginMode.Isolated: 1>
LEADER_FOLLOWER: FeedClockPolicy  # value = <FeedClockPolicy.LEADER_FOLLOWER: 2>
None_: MarkImpactModel  # value = <MarkImpactModel.None_: 0>
ONE_SIDED: OrderGroupPolicy  # value = <OrderGroupPolicy.ONE_SIDED: 2>
ORDER_FLAG_IOC: int = 4
ORDER_FLAG_POST_ONLY: int = 1
ORDER_FLAG_REDUCE_ONLY: int = 2
ORDER_TYPE_NAMES: tuple = ('limit', 'market', 'stop_market', 'stop_limit', 'tp_market', 'tp_limit', 'trailing_stop', 'iceberg')
PARTIALLY_FILLED: OrderGroupState  # value = <OrderGroupState.PARTIALLY_FILLED: 2>
PENDING: OrderGroupState  # value = <OrderGroupState.PENDING: 0>
PRICE_SCALE: int = 100000000
PnlRatio: AdlRanking  # value = <AdlRanking.PnlRatio: 0>
PositionSize: AdlRanking  # value = <AdlRanking.PositionSize: 3>
QUANTITY_SCALE: int = 100000000
QUEUE_FULL: int = 2
QUEUE_NONE: int = 0
QUEUE_TOB: int = 1
REVERTING: OrderGroupState  # value = <OrderGroupState.REVERTING: 5>
SIGNAL_FLAG_ENTER: int = 1
SIGNAL_FLAG_EXIT: int = 2
SIGNAL_FLAG_REBALANCE: int = 4
SLIPPAGE_FIXED_BPS: int = 2
SLIPPAGE_FIXED_TICKS: int = 1
SLIPPAGE_NONE: int = 0
SLIPPAGE_VOLUME_IMPACT: int = 3
SUBMITTED: OrderGroupState  # value = <OrderGroupState.SUBMITTED: 1>
VOLUME_SCALE: int = 100000000
WAIT_FOR_ALL: FeedClockPolicy  # value = <FeedClockPolicy.WAIT_FOR_ALL: 0>
StreamingIndicatorGraph = IndicatorGraph
