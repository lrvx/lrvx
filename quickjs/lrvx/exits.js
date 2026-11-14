// Exit rules for bar strategies: AtrStop, ChandelierExit, BreakEvenTrail,
// PercentStop, TakeProfit and TimeStop. Each instance holds one C handle
// (lrvx_exit_*) that owns the C++ rule of lrvx/strategy/exits.h, so the
// levels are the C++ arithmetic, bit for bit.
//
//   const stop = new AtrStop(2);
//   stop.arm('long', fillPrice, atr.value);     // when the position opens
//   if (stop.update(bar.high, bar.low, atr.value)) this.closePosition();
//   stop.stop, stop.armed, stop.disarm()
//
// update() tests the bar against the level resting before it, then moves the
// level. A hit does not disarm: it is reported until disarm(). The side is
// 'long' or 'short'; anything else throws. PercentStop and TakeProfit take
// arm(side, entryPrice) and update(high, low); TimeStop takes arm() and
// update(). destroy() releases the handle at once; an instance dropped
// without it is released when it is collected.

function _exitSide(side) {
    if (side === 'long') return 0;
    if (side === 'short') return 1;
    throw new RangeError("side must be 'long' or 'short'; got " + String(side));
}

function _exitLevel(v) { return (typeof v !== 'number' || isNaN(v)) ? null : v; }

class _ExitRule {
    constructor(h) { this._h = h; }
    get armed() { return __lrvx_exit_rule_armed(this._h); }
    disarm() { __lrvx_exit_rule_disarm(this._h); }
    destroy() { __lrvx_exit_rule_destroy(this._h); }
}

// The three ATR rules: arm(side, entryPrice, atr), update(high, low, atr).
class _AtrExitRule extends _ExitRule {
    arm(side, entryPrice, atr) { __lrvx_exit_rule_arm(this._h, _exitSide(side), entryPrice, atr); }
    update(high, low, atr) { return __lrvx_exit_rule_update(this._h, high, low, atr); }
    get stop() { return _exitLevel(__lrvx_exit_rule_level(this._h)); }
}

// A stop atrMultiple ATRs from the entry, frozen there.
class AtrStop extends _AtrExitRule {
    constructor(atrMultiple) {
        super(__lrvx_exit_atr_stop_create(atrMultiple));
        this.atrMultiple = atrMultiple;
    }
}

// The stop hangs atrMultiple ATRs below the highest high since the entry
// (above the lowest low for a short), looking back at most `lookback` bars;
// 0 looks back to the entry. It only moves in the position's favour.
class ChandelierExit extends _AtrExitRule {
    constructor(lookback, atrMultiple) {
        super(__lrvx_exit_chandelier_create(lookback >>> 0, atrMultiple));
        this.lookback = lookback >>> 0;
        this.atrMultiple = atrMultiple;
    }
}

// A hard stop hardStopAtr ATRs from the entry; once the position has moved
// breakEvenAtr ATRs in its favour the stop moves to the entry, then trails
// the best price at trailAtr ATRs.
class BreakEvenTrail extends _AtrExitRule {
    constructor(hardStopAtr, breakEvenAtr, trailAtr) {
        super(__lrvx_exit_break_even_trail_create(hardStopAtr, breakEvenAtr, trailAtr));
    }
    get atBreakEven() { return __lrvx_exit_rule_at_break_even(this._h); }
}

// A stop pct percent from the entry: entry * (1 - pct / 100) for a long,
// entry * (1 + pct / 100) for a short. pct must be > 0.
class PercentStop extends _ExitRule {
    constructor(pct) {
        super(__lrvx_exit_percent_stop_create(pct));
        this.pct = pct;
    }
    arm(side, entryPrice) { __lrvx_exit_rule_arm(this._h, _exitSide(side), entryPrice, 0); }
    update(high, low) { return __lrvx_exit_rule_update(this._h, high, low, 0); }
    get stop() { return _exitLevel(__lrvx_exit_rule_level(this._h)); }
}

// A profit target pct percent from the entry: entry * (1 + pct / 100) for a
// long (reached when high >= target), entry * (1 - pct / 100) for a short.
class TakeProfit extends _ExitRule {
    constructor(pct) {
        super(__lrvx_exit_take_profit_create(pct));
        this.pct = pct;
    }
    arm(side, entryPrice) { __lrvx_exit_rule_arm(this._h, _exitSide(side), entryPrice, 0); }
    update(high, low) { return __lrvx_exit_rule_update(this._h, high, low, 0); }
    get target() { return _exitLevel(__lrvx_exit_rule_level(this._h)); }
}

// Exit after maxBars bars: update() counts the bars since arm() and is true
// from the maxBars-th one on. maxBars must be >= 1.
class TimeStop extends _ExitRule {
    constructor(maxBars) {
        super(__lrvx_exit_time_stop_create(maxBars >>> 0));
        this.maxBars = maxBars >>> 0;
    }
    arm() { __lrvx_exit_rule_arm(this._h, 0, 0, 0); }
    update() { return __lrvx_exit_rule_update(this._h, 0, 0, 0); }
    get bars() { return __lrvx_exit_rule_bars(this._h); }
}
