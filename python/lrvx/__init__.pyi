"""
lrvx Python bindings.

The compiled pybind11 extension lives at `lrvx._lrvx` and is
re-exported here so users can `import lrvx` and access the full
public surface (indicators, engine, backtest, replay, etc.) at the
top level. Type information is shipped via `lrvx/__init__.pyi`
and the PEP 561 marker `lrvx/py.typed`.
"""
from . import _lrvx as _lrvx
from . import custom_venue
from . import live_queue_calibrator
from __future__ import annotations
from lrvx._lrvx import Account
from lrvx._lrvx import adf
from lrvx._lrvx import AdlRanking
from lrvx._lrvx import adx
from lrvx._lrvx import aggregate_heikin_ashi_bars
from lrvx._lrvx import aggregate_range_bars
from lrvx._lrvx import aggregate_renko_bars
from lrvx._lrvx import aggregate_tick_bars
from lrvx._lrvx import aggregate_time_bars
from lrvx._lrvx import aggregate_volume_bars
from lrvx._lrvx import AggregatorEventFilter
from lrvx._lrvx import AmmCurve
from lrvx._lrvx import ATR
from lrvx._lrvx import atr
from lrvx._lrvx import AutoCorrelation
from lrvx._lrvx import autocorrelation
from lrvx._lrvx import BacktestResult
from lrvx._lrvx import BacktestRunner
from lrvx._lrvx import bar_returns
from lrvx._lrvx import BarData
from lrvx._lrvx import BarDispatchRecorder
from lrvx._lrvx import baw_price
from lrvx._lrvx import BinaryLogRecorderHook
from lrvx._lrvx import BinCountAggregator
from lrvx._lrvx import binomial_price
from lrvx._lrvx import Bollinger
from lrvx._lrvx import bollinger
from lrvx._lrvx import BookSnapshotBinAggregator
from lrvx._lrvx import bootstrap_ci
from lrvx._lrvx import bs_price
from lrvx._lrvx import bs_price_discrete_dividends
from lrvx._lrvx import bs_vega
from lrvx._lrvx import build_surface_as_of
from lrvx._lrvx import calibrate_svi
from lrvx._lrvx import capi_abi_version
from lrvx._lrvx import CCI
from lrvx._lrvx import cci
from lrvx._lrvx import chop
from lrvx._lrvx import CompositeBookMatrix
from lrvx._lrvx import ConstantLatency
from lrvx._lrvx import Correlation
from lrvx._lrvx import correlation
from lrvx._lrvx import cost_of_carry
from lrvx._lrvx import cvd
from lrvx._lrvx import DataReader
from lrvx._lrvx import DataWriter
from lrvx._lrvx import deflated_sharpe
from lrvx._lrvx import DeltaBookEncoder
from lrvx._lrvx import DeltaBookReplayer
from lrvx._lrvx import DEMA
from lrvx._lrvx import dema
from lrvx._lrvx import EMA
from lrvx._lrvx import ema
from lrvx._lrvx import EmpiricalLatency
from lrvx._lrvx import Engine
from lrvx._lrvx import EventTypeStatsAggregator
from lrvx._lrvx import ExchangeCapabilities
from lrvx._lrvx import ExecutionListener
from lrvx._lrvx import Executor
from lrvx._lrvx import ExponentialLatency
from lrvx._lrvx import export_data
from lrvx._lrvx import extract_symbols
from lrvx._lrvx import extract_time_range
from lrvx._lrvx import FeedClockPolicy
from lrvx._lrvx import FeeSchedule
from lrvx._lrvx import FeeTier
from lrvx._lrvx import FillLiquidity
from lrvx._lrvx import FootprintBar
from lrvx._lrvx import forward_price
from lrvx._lrvx import FundingPayment
from lrvx._lrvx import FundingSchedule
from lrvx._lrvx import FundingTapeEntry
from lrvx._lrvx import GaussianLatency
from lrvx._lrvx import greeks
from lrvx._lrvx import grid_neighborhood
from lrvx._lrvx import GridSearch
from lrvx._lrvx import hurst_dfa
from lrvx._lrvx import i256_roundtrip
from lrvx._lrvx import implied_percentile_in_cone
from lrvx._lrvx import implied_vol
from lrvx._lrvx import IndicatorGraph
from lrvx._lrvx import IndicatorGraph as StreamingIndicatorGraph
from lrvx._lrvx import inspect
from lrvx._lrvx import KAMA
from lrvx._lrvx import kama
from lrvx._lrvx import KillSwitch
from lrvx._lrvx import Kurtosis
from lrvx._lrvx import kurtosis
from lrvx._lrvx import L3Book
from lrvx._lrvx import LatencyDistribution
from lrvx._lrvx import LatencyModel
from lrvx._lrvx import LatencySample
from lrvx._lrvx import LegState
from lrvx._lrvx import LiquidationEngine
from lrvx._lrvx import list_indicators
from lrvx._lrvx import LiveQueuePositionEstimator
from lrvx._lrvx import LrvxError
from lrvx._lrvx import MACD
from lrvx._lrvx import macd
from lrvx._lrvx import MarginMode
from lrvx._lrvx import MarketDataRecorderHook
from lrvx._lrvx import MarketProfile
from lrvx._lrvx import MarkImpactModel
from lrvx._lrvx import merge
from lrvx._lrvx import merge_dir
from lrvx._lrvx import MergedTapeReader
from lrvx._lrvx import MultiFeedClock
from lrvx._lrvx import obv
from lrvx._lrvx import OHLCBinAggregator
from lrvx._lrvx import OptionType
from lrvx._lrvx import Order
from lrvx._lrvx import order_type_code
from lrvx._lrvx import order_type_name
from lrvx._lrvx import OrderBook
from lrvx._lrvx import OrderEventData
from lrvx._lrvx import OrderEventKind
from lrvx._lrvx import OrderGroup
from lrvx._lrvx import OrderGroupPolicy
from lrvx._lrvx import OrderGroupState
from lrvx._lrvx import OrderJourneyTracer
from lrvx._lrvx import OrderTracker
from lrvx._lrvx import OrderValidator
from lrvx._lrvx import parkinson_vol
from lrvx._lrvx import ParkinsonVol
from lrvx._lrvx import Partitioner
from lrvx._lrvx import pbo_cscv
from lrvx._lrvx import PeakAggregator
from lrvx._lrvx import permutation_test
from lrvx._lrvx import PnLTracker
from lrvx._lrvx import PoolReplay
from lrvx._lrvx import PoolTape
from lrvx._lrvx import PositionGroupTracker
from lrvx._lrvx import PositionTracker
from lrvx._lrvx import prices_to_double
from lrvx._lrvx import profit_factor
from lrvx._lrvx import QuantileAggregator
from lrvx._lrvx import quantities_to_double
from lrvx._lrvx import RateLimitEndpointFamily
from lrvx._lrvx import RateLimitPolicy
from lrvx._lrvx import realized_vol
from lrvx._lrvx import recompress
from lrvx._lrvx import ReplayEvent
from lrvx._lrvx import ReplaySource
from lrvx._lrvx import RiskManager
from lrvx._lrvx import RMA
from lrvx._lrvx import rma
from lrvx._lrvx import rogers_satchell_vol
from lrvx._lrvx import RogersSatchellVol
from lrvx._lrvx import rolling_correlation
from lrvx._lrvx import rolling_hurst
from lrvx._lrvx import rolling_zscore
from lrvx._lrvx import RollingZScore
from lrvx._lrvx import RSI
from lrvx._lrvx import rsi
from lrvx._lrvx import Runner
from lrvx._lrvx import second_order_greeks
from lrvx._lrvx import set_log_callback
from lrvx._lrvx import shannon_entropy
from lrvx._lrvx import ShannonEntropy
from lrvx._lrvx import Signal
from lrvx._lrvx import SignalBuilder
from lrvx._lrvx import SimulatedClock
from lrvx._lrvx import SimulatedExecutor
from lrvx._lrvx import Skewness
from lrvx._lrvx import skewness
from lrvx._lrvx import Slope
from lrvx._lrvx import slope
from lrvx._lrvx import SMA
from lrvx._lrvx import sma
from lrvx._lrvx import split
from lrvx._lrvx import Stats
from lrvx._lrvx import Stochastic
from lrvx._lrvx import stochastic
from lrvx._lrvx import StorageSink
from lrvx._lrvx import Strategy
from lrvx._lrvx import Symbol
from lrvx._lrvx import SymbolContext
from lrvx._lrvx import SymbolRegistry
from lrvx._lrvx import TapeRef
from lrvx._lrvx import targets
from lrvx._lrvx import TEMA
from lrvx._lrvx import tema
from lrvx._lrvx import TraceReader
from lrvx._lrvx import TraceRecorder
from lrvx._lrvx import trade_pnl
from lrvx._lrvx import TradeData
from lrvx._lrvx import u256_from_hex
from lrvx._lrvx import u256_roundtrip
from lrvx._lrvx import validate
from lrvx._lrvx import validate_dataset
from lrvx._lrvx import VenueAvailability
from lrvx._lrvx import VenueExecutor
from lrvx._lrvx import VenueStack
from lrvx._lrvx import vol_cone
from lrvx._lrvx import VolSurface
from lrvx._lrvx import VolumeBinAggregator
from lrvx._lrvx import VolumeProfile
from lrvx._lrvx import volumes_to_double
from lrvx._lrvx import vwap
from lrvx._lrvx import WalkForwardRunner
from lrvx._lrvx import whites_reality_check
from lrvx._lrvx import win_rate
from lrvx.custom_venue import assemble_custom_venue
from lrvx.custom_venue import CustomVenue
from lrvx.live_queue_calibrator import CalibrationResult
from lrvx.live_queue_calibrator import LiveQueueCalibrator
from lrvx.live_queue_calibrator import TestOrderHelper
__all__: list[str] = ['_lrvx', 'ALL_OR_NOTHING', 'ATR', 'Account', 'AdlRanking', 'AggregatorEventFilter', 'AmmCurve', 'AutoCorrelation', 'BEST_EFFORT', 'BacktestResult', 'BacktestRunner', 'BarData', 'BarDispatchRecorder', 'BinCountAggregator', 'Binance', 'BinaryLogRecorderHook', 'Bollinger', 'BookAnchored', 'BookOnly', 'BookSnapshotBinAggregator', 'Bybit', 'CANCELLED', 'CAPI_ABI_VERSION', 'CCI', 'CalibrationResult', 'CompositeBookMatrix', 'ConstantLatency', 'Correlation', 'Cross', 'CustomVenue', 'DEMA', 'DataReader', 'DataWriter', 'DeltaBookEncoder', 'DeltaBookReplayer', 'EMA', 'EmpiricalLatency', 'Engine', 'EventTypeStatsAggregator', 'ExchangeCapabilities', 'ExecutionListener', 'Executor', 'ExponentialLatency', 'FAILED', 'FILLED', 'FIRE_ON_ANY', 'FeeSchedule', 'FeeTier', 'FeedClockPolicy', 'FillLiquidity', 'FootprintBar', 'FundingPayment', 'FundingSchedule', 'FundingTapeEntry', 'GaussianLatency', 'GridSearch', 'IndicatorGraph', 'Isolated', 'KAMA', 'KillSwitch', 'Kurtosis', 'L3Book', 'LEADER_FOLLOWER', 'LatencyDistribution', 'LatencyModel', 'LatencySample', 'LegState', 'LiquidationEngine', 'LiveQueueCalibrator', 'LiveQueuePositionEstimator', 'LrvxError', 'MACD', 'MarginMode', 'MarkImpactModel', 'MarketDataRecorderHook', 'MarketProfile', 'MergedTapeReader', 'MultiFeedClock', 'None_', 'OHLCBinAggregator', 'ONE_SIDED', 'ORDER_FLAG_IOC', 'ORDER_FLAG_POST_ONLY', 'ORDER_FLAG_REDUCE_ONLY', 'ORDER_TYPE_NAMES', 'OptionType', 'Order', 'OrderBook', 'OrderEventData', 'OrderEventKind', 'OrderGroup', 'OrderGroupPolicy', 'OrderGroupState', 'OrderJourneyTracer', 'OrderTracker', 'OrderValidator', 'PARTIALLY_FILLED', 'PENDING', 'PRICE_SCALE', 'ParkinsonVol', 'Partitioner', 'PeakAggregator', 'PnLTracker', 'PnlRatio', 'PoolReplay', 'PoolTape', 'PositionGroupTracker', 'PositionSize', 'PositionTracker', 'QUANTITY_SCALE', 'QUEUE_FULL', 'QUEUE_NONE', 'QUEUE_TOB', 'QuantileAggregator', 'REVERTING', 'RMA', 'RSI', 'RateLimitEndpointFamily', 'RateLimitPolicy', 'ReplayEvent', 'ReplaySource', 'RiskManager', 'RogersSatchellVol', 'RollingZScore', 'Runner', 'SIGNAL_FLAG_ENTER', 'SIGNAL_FLAG_EXIT', 'SIGNAL_FLAG_REBALANCE', 'SLIPPAGE_FIXED_BPS', 'SLIPPAGE_FIXED_TICKS', 'SLIPPAGE_NONE', 'SLIPPAGE_VOLUME_IMPACT', 'SMA', 'SUBMITTED', 'ShannonEntropy', 'Signal', 'SignalBuilder', 'SimulatedClock', 'SimulatedExecutor', 'Skewness', 'Slope', 'Stats', 'Stochastic', 'StorageSink', 'Strategy', 'StreamingIndicatorGraph', 'Symbol', 'SymbolContext', 'SymbolRegistry', 'TEMA', 'TapeRef', 'TestOrderHelper', 'TraceReader', 'TraceRecorder', 'TradeData', 'VOLUME_SCALE', 'VenueAvailability', 'VenueExecutor', 'VenueStack', 'VolSurface', 'VolumeBinAggregator', 'VolumeProfile', 'WAIT_FOR_ALL', 'WalkForwardRunner', 'adf', 'adx', 'aggregate_heikin_ashi_bars', 'aggregate_range_bars', 'aggregate_renko_bars', 'aggregate_tick_bars', 'aggregate_time_bars', 'aggregate_volume_bars', 'assemble_custom_venue', 'atr', 'autocorrelation', 'bar_returns', 'baw_price', 'binomial_price', 'bollinger', 'bootstrap_ci', 'bs_price', 'bs_price_discrete_dividends', 'bs_vega', 'build_surface_as_of', 'calibrate_svi', 'capi_abi_version', 'cci', 'chop', 'correlation', 'cost_of_carry', 'custom_venue', 'cvd', 'deflated_sharpe', 'dema', 'ema', 'export_data', 'extract_symbols', 'extract_time_range', 'forward_price', 'greeks', 'grid_neighborhood', 'hurst_dfa', 'i256_roundtrip', 'implied_percentile_in_cone', 'implied_vol', 'inspect', 'kama', 'kurtosis', 'list_indicators', 'live_queue_calibrator', 'macd', 'merge', 'merge_dir', 'obv', 'order_type_code', 'order_type_name', 'parkinson_vol', 'pbo_cscv', 'permutation_test', 'prices_to_double', 'profit_factor', 'quantities_to_double', 'realized_vol', 'recompress', 'rma', 'rogers_satchell_vol', 'rolling_correlation', 'rolling_hurst', 'rolling_zscore', 'rsi', 'second_order_greeks', 'set_log_callback', 'shannon_entropy', 'skewness', 'slope', 'sma', 'split', 'stochastic', 'targets', 'tema', 'trade_pnl', 'u256_from_hex', 'u256_roundtrip', 'validate', 'validate_dataset', 'vol_cone', 'volumes_to_double', 'vwap', 'whites_reality_check', 'win_rate']
ALL_OR_NOTHING: _lrvx.OrderGroupPolicy  # value = <OrderGroupPolicy.ALL_OR_NOTHING: 1>
BEST_EFFORT: _lrvx.OrderGroupPolicy  # value = <OrderGroupPolicy.BEST_EFFORT: 0>
Binance: _lrvx.AdlRanking  # value = <AdlRanking.Binance: 1>
BookAnchored: _lrvx.MarkImpactModel  # value = <MarkImpactModel.BookAnchored: 1>
BookOnly: _lrvx.MarkImpactModel  # value = <MarkImpactModel.BookOnly: 2>
Bybit: _lrvx.AdlRanking  # value = <AdlRanking.Bybit: 2>
CANCELLED: _lrvx.OrderGroupState  # value = <OrderGroupState.CANCELLED: 4>
CAPI_ABI_VERSION: int = 5
Cross: _lrvx.MarginMode  # value = <MarginMode.Cross: 0>
FAILED: _lrvx.OrderGroupState  # value = <OrderGroupState.FAILED: 6>
FILLED: _lrvx.OrderGroupState  # value = <OrderGroupState.FILLED: 3>
FIRE_ON_ANY: _lrvx.FeedClockPolicy  # value = <FeedClockPolicy.FIRE_ON_ANY: 1>
Isolated: _lrvx.MarginMode  # value = <MarginMode.Isolated: 1>
LEADER_FOLLOWER: _lrvx.FeedClockPolicy  # value = <FeedClockPolicy.LEADER_FOLLOWER: 2>
None_: _lrvx.MarkImpactModel  # value = <MarkImpactModel.None_: 0>
ONE_SIDED: _lrvx.OrderGroupPolicy  # value = <OrderGroupPolicy.ONE_SIDED: 2>
ORDER_FLAG_IOC: int = 4
ORDER_FLAG_POST_ONLY: int = 1
ORDER_FLAG_REDUCE_ONLY: int = 2
ORDER_TYPE_NAMES: tuple = ('limit', 'market', 'stop_market', 'stop_limit', 'tp_market', 'tp_limit', 'trailing_stop', 'iceberg')
PARTIALLY_FILLED: _lrvx.OrderGroupState  # value = <OrderGroupState.PARTIALLY_FILLED: 2>
PENDING: _lrvx.OrderGroupState  # value = <OrderGroupState.PENDING: 0>
PRICE_SCALE: int = 100000000
PnlRatio: _lrvx.AdlRanking  # value = <AdlRanking.PnlRatio: 0>
PositionSize: _lrvx.AdlRanking  # value = <AdlRanking.PositionSize: 3>
QUANTITY_SCALE: int = 100000000
QUEUE_FULL: int = 2
QUEUE_NONE: int = 0
QUEUE_TOB: int = 1
REVERTING: _lrvx.OrderGroupState  # value = <OrderGroupState.REVERTING: 5>
SIGNAL_FLAG_ENTER: int = 1
SIGNAL_FLAG_EXIT: int = 2
SIGNAL_FLAG_REBALANCE: int = 4
SLIPPAGE_FIXED_BPS: int = 2
SLIPPAGE_FIXED_TICKS: int = 1
SLIPPAGE_NONE: int = 0
SLIPPAGE_VOLUME_IMPACT: int = 3
SUBMITTED: _lrvx.OrderGroupState  # value = <OrderGroupState.SUBMITTED: 1>
VOLUME_SCALE: int = 100000000
WAIT_FOR_ALL: _lrvx.FeedClockPolicy  # value = <FeedClockPolicy.WAIT_FOR_ALL: 0>
