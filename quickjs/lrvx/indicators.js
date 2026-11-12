// quickjs/lrvx/indicators.js
//
// Each indicator is ONE class with both batch and streaming API on the
// same instance.
//
//   const ema = new EMA(10);
//   const out = EMA.compute(prices);          // batch
//   for (const v of stream) {                 // streaming on the same instance
//     ema.update(v);
//     if (ema.ready) console.log(ema.value);
//   }
//
// Streaming: each instance holds one C handle (lrvx_streaming_indicator_*)
// that owns the C++ streaming indicator. update() is one kernel step of that
// class, so its cost does not depend on how many values the instance has
// seen, and values, readiness and NaN behaviour are the C++ class's. A value
// that is NaN reads as null. destroy() releases the handle at once; an
// instance dropped without destroy() is released by the QuickJS finalizer
// when it is collected, or when the runtime is torn down.

// ── Helpers ────────────────────────────────────────────────────────────

function _optNum(v) { return (typeof v !== 'number' || isNaN(v)) ? null : v; }

// Integer constructor arguments convert the way the batch bindings convert
// a period (ToUint32), so a period such as 14.0 or "14" means what it did.
function _u(v) { return v >>> 0; }

// Streaming base: the handle, and everything that does not depend on the
// shape of an update.
class _StreamHandle {
    constructor(name, args) { this._h = __lrvx_streaming_indicator_create(name, args); }
    get value() { return _optNum(__lrvx_streaming_indicator_value(this._h)); }
    get ready() { return __lrvx_streaming_indicator_ready(this._h); }
    get count() { return __lrvx_streaming_indicator_count(this._h); }
    reset() { __lrvx_streaming_indicator_reset(this._h); }
    destroy() { __lrvx_streaming_indicator_destroy(this._h); }
    _output(i) { return _optNum(__lrvx_streaming_indicator_output(this._h, i)); }
}

// Single-input: update(v).
class _Stream1 extends _StreamHandle {
    update(v) { return _optNum(__lrvx_streaming_indicator_update(this._h, v)); }
}

// Bar-input: update(high, low, close).
class _StreamBar extends _StreamHandle {
    update(high, low, close) {
        return _optNum(__lrvx_streaming_indicator_update_bar(this._h, high, low, close));
    }
}

// High/low: update(high, low).
class _StreamHighLow extends _StreamHandle {
    update(high, low) {
        return _optNum(__lrvx_streaming_indicator_update_high_low(this._h, high, low));
    }
}

// OHLC: update(open, high, low, close).
class _StreamOhlc extends _StreamHandle {
    update(open, high, low, close) {
        return _optNum(__lrvx_streaming_indicator_update_ohlc(this._h, open, high, low, close));
    }
}

// Pair: update(x, y).
class _StreamPair extends _StreamHandle {
    update(x, y) { return _optNum(__lrvx_streaming_indicator_update_pair(this._h, x, y)); }
}

// ============================================================
// Moving averages / oscillators
// ============================================================

class SMA extends _Stream1 {
    constructor(period) { super('sma', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_sma(data, period); }
}

class EMA extends _Stream1 {
    constructor(period) { super('ema', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_ema(data, period); }
}

class RMA extends _Stream1 {
    constructor(period) { super('rma', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_rma(data, period); }
}

class DEMA extends _Stream1 {
    constructor(period) { super('dema', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_dema(data, period); }
}

class TEMA extends _Stream1 {
    constructor(period) { super('tema', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_tema(data, period); }
}

class RSI extends _Stream1 {
    constructor(period) { super('rsi', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_rsi(data, period); }
}

class Slope extends _Stream1 {
    constructor(length) { super('slope', [_u(length)]); }
    static compute(data, length) { return __lrvx_indicator_slope(data, length); }
}

// KAMA(period, fast=2, slow=30)
class KAMA extends _Stream1 {
    constructor(period, fast = 2, slow = 30) {
        super('kama', [_u(period), _u(fast), _u(slow)]);
        this._period = period; this._fast = fast; this._slow = slow;
    }
    compute(data) { return __lrvx_indicator_kama(data, this._period, this._fast, this._slow); }
    static compute(data, period, fast = 2, slow = 30) {
        return __lrvx_indicator_kama(data, period, fast, slow);
    }
}

// ============================================================
// Bar-input
// ============================================================

class ATR extends _StreamBar {
    constructor(period) { super('atr', [_u(period)]); }
    static compute(high, low, close, period) {
        return __lrvx_indicator_atr(high, low, close, period);
    }
}

class CCI extends _StreamBar {
    constructor(period) { super('cci', [_u(period)]); }
    static compute(high, low, close, period) {
        return __lrvx_indicator_cci(high, low, close, period);
    }
}

class CHOP extends _StreamBar {
    constructor(period) { super('chop', [_u(period)]); }
    static compute(high, low, close, period) {
        return __lrvx_indicator_chop(high, low, close, period);
    }
}

// ADX returns a struct {adx, plus_di, minus_di} — keep batch-only
class ADX {
    static compute(high, low, close, period) {
        return __lrvx_indicator_adx(high, low, close, period);
    }
}

// ============================================================
// Multi-output (line/signal/histogram, upper/middle/lower, k/d)
// ============================================================

// MACD(fast, slow, signal) — multi-output: value/line, signal, histogram
class MACD extends _Stream1 {
    constructor(fast = 12, slow = 26, signal = 9) {
        super('macd', [_u(fast), _u(slow), _u(signal)]);
        this._fast = fast; this._slow = slow; this._signal = signal;
    }
    get line() { return this.value; }
    get signal() { return this._output(1); }
    get histogram() { return this._output(2); }
    compute(data) { return __lrvx_indicator_macd(data, this._fast, this._slow, this._signal); }
    static compute(data, fast = 12, slow = 26, signal = 9) {
        return __lrvx_indicator_macd(data, fast, slow, signal);
    }
}

// Bollinger(period, multiplier) — value/middle, upper, lower
class Bollinger extends _Stream1 {
    constructor(period = 20, multiplier = 2.0) {
        super('bollinger', [_u(period), +multiplier]);
        this._period = period; this._mul = multiplier;
    }
    get middle() { return this.value; }
    get upper() { return this._output(1); }
    get lower() { return this._output(2); }
    compute(data) { return __lrvx_indicator_bollinger(data, this._period, this._mul); }
    static compute(data, period = 20, multiplier = 2.0) {
        return __lrvx_indicator_bollinger(data, period, multiplier);
    }
}

// Stochastic(kPeriod, dPeriod) — value/k, d. ready once both are finite.
class Stochastic extends _StreamBar {
    constructor(kPeriod = 14, dPeriod = 3) {
        super('stochastic', [_u(kPeriod), _u(dPeriod)]);
        this._k = kPeriod; this._d = dPeriod;
    }
    get k() { return this.value; }
    get d() { return this._output(1); }
    compute(high, low, close) { return __lrvx_indicator_stochastic(high, low, close, this._k, this._d); }
    static compute(high, low, close, kPeriod = 14, dPeriod = 3) {
        return __lrvx_indicator_stochastic(high, low, close, kPeriod, dPeriod);
    }
}

// ============================================================
// Volume / cumulative — batch-only static helpers
// ============================================================

class OBV {
    static compute(close, volume) { return __lrvx_indicator_obv(close, volume); }
}
class VWAP {
    static compute(close, volume, window) { return __lrvx_indicator_vwap(close, volume, window); }
}
class CVD {
    static compute(open, high, low, close, volume) {
        return __lrvx_indicator_cvd(open, high, low, close, volume);
    }
}

// ============================================================
// Statistical / volatility
// ============================================================

class Skewness extends _Stream1 {
    constructor(period) { super('skewness', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_skewness(data, period); }
}

class Kurtosis extends _Stream1 {
    constructor(period) { super('kurtosis', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_kurtosis(data, period); }
}

class RollingZScore extends _Stream1 {
    constructor(period) { super('rolling_zscore', [_u(period)]); }
    static compute(data, period) { return __lrvx_indicator_rolling_zscore(data, period); }
}

// ShannonEntropy(period, bins)
class ShannonEntropy extends _Stream1 {
    constructor(period, bins = 10) {
        super('shannon_entropy', [_u(period), _u(bins)]);
        this._period = period; this._bins = bins;
    }
    compute(data) { return __lrvx_indicator_shannon_entropy(data, this._period, this._bins); }
    static compute(data, period, bins = 10) {
        return __lrvx_indicator_shannon_entropy(data, period, bins);
    }
}

class ParkinsonVol extends _StreamHighLow {
    constructor(period) { super('parkinson_vol', [_u(period)]); }
    static compute(high, low, period) { return __lrvx_indicator_parkinson_vol(high, low, period); }
}

class RogersSatchellVol extends _StreamOhlc {
    constructor(period) { super('rogers_satchell_vol', [_u(period)]); }
    static compute(open, high, low, close, period) {
        return __lrvx_indicator_rogers_satchell_vol(open, high, low, close, period);
    }
}

class Correlation extends _StreamPair {
    constructor(period) { super('correlation', [_u(period)]); }
    static compute(x, y, period) { return __lrvx_indicator_correlation(x, y, period); }
}

// ============================================================
// Stationarity tests
// ============================================================

// Augmented Dickey-Fuller test. Returns { test_stat, p_value, used_lag }.
function adf(data, max_lag, regression) {
    return __lrvx_indicator_adf(data, max_lag === undefined ? 4 : max_lag,
                                regression === undefined ? "c" : regression);
}

// ============================================================
// AutoCorrelation
// ============================================================

class AutoCorrelation extends _Stream1 {
    constructor(window, lag) {
        super('autocorrelation', [_u(window), _u(lag)]);
        this._window = window; this._lag = lag;
    }
    compute(data) { return __lrvx_indicator_autocorrelation(data, this._window, this._lag); }
    static compute(data, window, lag) { return __lrvx_indicator_autocorrelation(data, window, lag); }
}
