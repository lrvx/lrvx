// History(capacity): the last `capacity` values of a series, newest first,
// so get(0) is the value pushed last and get(1) the one before. Holds one C
// handle (lrvx_history_*) owning the C++ lrvx::History.
//
//   const closes = new History(3);
//   closes.push(bar.close);
//   if (closes.full && closes.get(0) > closes.get(2)) { ... }
//
// crossedAbove(aPrev, a, bPrev, b): a was at or below b on the previous step
// and is strictly above it now; crossedBelow the mirror. Compare against a
// level by passing it as both bPrev and b. A NaN, null or undefined anywhere
// is no cross.

class History {
    constructor(capacity) {
        if (typeof capacity !== 'number' || !(capacity >= 1) || capacity !== Math.floor(capacity)) {
            throw new RangeError('History capacity must be a whole number >= 1; got ' + String(capacity));
        }
        this._h = __lrvx_history_create(capacity);
    }
    push(value) { __lrvx_history_push(this._h, value); }
    // Throws RangeError for an index outside 0 .. length - 1.
    get(index) { return __lrvx_history_get(this._h, index); }
    get length() { return __lrvx_history_size(this._h); }
    get capacity() { return __lrvx_history_capacity(this._h); }
    get full() { return __lrvx_history_full(this._h); }
    destroy() { __lrvx_history_destroy(this._h); }
}

function _crossArgs(a, b, c, d) {
    return typeof a === 'number' && typeof b === 'number' &&
           typeof c === 'number' && typeof d === 'number';
}

function crossedAbove(aPrev, a, bPrev, b) {
    return _crossArgs(aPrev, a, bPrev, b) && __lrvx_crossed_above(aPrev, a, bPrev, b);
}

function crossedBelow(aPrev, a, bPrev, b) {
    return _crossArgs(aPrev, a, bPrev, b) && __lrvx_crossed_below(aPrev, a, bPrev, b);
}
