var SIDE_MAP = { buy: 0, sell: 1 };
var TIF_MAP = { GTC: 0, IOC: 1, FOK: 2, GTD: 3, POST_ONLY: 4 };

function _resolveSide(side) {
    var v = SIDE_MAP[side];
    if (v === undefined) throw new Error("Invalid side: '" + side + "'. Use 'buy' or 'sell'.");
    return v;
}

// An order id is the Number an order method returned and order events carry,
// or a BigInt. NaN, a fraction, a negative or a string used to be converted
// to some integer and a cancel or modify sent for whatever order that named.
function _orderId(usage, orderId) {
    if (typeof orderId === "bigint") {
        if (orderId >= 0n && orderId <= 0xFFFFFFFFFFFFFFFFn) return orderId;
    } else if (typeof orderId === "number") {
        if (orderId >= 0 && orderId <= 9007199254740992 && Math.floor(orderId) === orderId) {
            return orderId;
        }
    }
    throw new TypeError(usage +
        " expected: orderId is a non-negative integer Number or a BigInt");
}

function _resolveTif(tif) {
    if (tif === undefined || tif === null) return 0;
    var v = TIF_MAP[tif];
    if (v === undefined) throw new Error("Invalid tif: '" + tif + "'. Use GTC/IOC/FOK/POST_ONLY.");
    return v;
}

class Strategy {
    constructor(config) {
        this._exchange = config.exchange || null;
        this._symbolNames = config.symbols;
        this._symbolMap = {};
        this._reverseMap = {};
        this._handle = null;
    }

    onTrade(ctx, trade) {}
    onBookUpdate(ctx, book) {}
    onTouch(ctx, book) {}
    onBar(ctx, bar) {}
    onFill(ctx, ev) {}
    onOrderUpdate(ctx, ev) {}
    onQueuePositionChange(ctx, ev) {}
    onMarketPositionChange(ctx, ev) {}
    onStart() {}
    onStop() {}

    // --- Market orders ---

    marketBuy(opts) {
        return __lrvx_emit_market_buy(this._handle, this._resolve(opts.symbol), opts.qty);
    }
    marketSell(opts) {
        return __lrvx_emit_market_sell(this._handle, this._resolve(opts.symbol), opts.qty);
    }

    // --- Limit orders ---

    limitBuy(opts) {
        var sym = this._resolve(opts.symbol);
        var tif = _resolveTif(opts.tif);
        return __lrvx_emit_limit_buy_tif(this._handle, sym, opts.price, opts.qty, tif);
    }
    limitSell(opts) {
        var sym = this._resolve(opts.symbol);
        var tif = _resolveTif(opts.tif);
        return __lrvx_emit_limit_sell_tif(this._handle, sym, opts.price, opts.qty, tif);
    }

    // --- Stop orders ---

    stopMarket(opts) {
        return __lrvx_emit_stop_market(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.qty);
    }
    stopLimit(opts) {
        return __lrvx_emit_stop_limit(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.price, opts.qty);
    }

    // --- Take profit orders ---

    takeProfitMarket(opts) {
        return __lrvx_emit_take_profit_market(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.qty);
    }
    takeProfitLimit(opts) {
        return __lrvx_emit_take_profit_limit(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.trigger, opts.price, opts.qty);
    }

    // --- Trailing stop orders ---

    trailingStop(opts) {
        return __lrvx_emit_trailing_stop(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.offset, opts.qty);
    }
    trailingStopPercent(opts) {
        return __lrvx_emit_trailing_stop_pct(this._handle, this._resolve(opts.symbol),
            _resolveSide(opts.side), opts.callbackBps, opts.qty);
    }

    // --- Order management ---

    cancel(orderId) {
        __lrvx_emit_cancel(this._handle, _orderId("cancel(orderId)", orderId));
    }
    cancelAll(symbol) { __lrvx_emit_cancel_all(this._handle, this._resolve(symbol)); }
    modify(orderId, opts) {
        __lrvx_emit_modify(this._handle, _orderId("modify(orderId, opts)", orderId),
            opts.price, opts.qty);
    }
    closePosition(symbol) {
        return __lrvx_emit_close_position(this._handle, this._resolve(symbol));
    }

    // --- Context queries ---

    position(symbol) { return __lrvx_position(this._handle, this._resolve(symbol)); }
    lastPrice(symbol) { return __lrvx_last_trade_price(this._handle, this._resolve(symbol)); }
    bestBid(symbol) { return __lrvx_best_bid(this._handle, this._resolve(symbol)); }
    bestAsk(symbol) { return __lrvx_best_ask(this._handle, this._resolve(symbol)); }
    midPrice(symbol) { return __lrvx_mid_price(this._handle, this._resolve(symbol)); }
    orderStatus(orderId) { return __lrvx_get_order_status(this._handle, orderId); }

    // --- Multi-timeframe alignment helpers ---
    //
    // The engine pushes each closed bar into a per-(symbol, type, param) ring as
    // it dispatches BarEvents. `lastClosedBar` returns the most recent or null;
    // `lastNClosedBars` returns up to `n` bars oldest-first. `barType` is 0=Time,
    // 1=Tick, 2=Volume, ...; `param` matches BarEvent.barTypeParam (interval-ns
    // for Time bars, count for Tick bars, threshold for Volume bars).

    lastClosedBar(symbol, barType, param) {
        return __lrvx_strategy_last_closed_bar(this._handle, this._resolve(symbol),
            barType, param);
    }
    lastNClosedBars(symbol, barType, param, n) {
        return __lrvx_strategy_last_n_closed_bars(this._handle, this._resolve(symbol),
            barType, param, n);
    }
    get barRingCapacity() {
        return __lrvx_strategy_get_bar_ring_capacity(this._handle);
    }
    setBarRingCapacity(n) {
        __lrvx_strategy_set_bar_ring_capacity(this._handle, n);
    }

    // --- Properties ---

    get primarySymbol() { return this._symbolNames[0]; }
    get hasPosition() { return this.position() !== 0; }
    get symbols() { return this._symbolNames; }

    // --- Internal ---

    _resolve(symbol) {
        var name = symbol !== undefined && symbol !== null ? symbol : this._symbolNames[0];
        // Numeric symbols pass through unchanged so the composite DSL can
        // pass `ctx.symbolId` directly without re-stringifying.
        if (typeof name === 'number') return name;
        var id = this._symbolMap[name];
        if (id === undefined) {
            throw new Error("Unknown symbol: " + name);
        }
        return id;
    }

    _dispatchTrade(rawCtx, rawTrade) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        rawTrade.symbol = this._reverseMap[rawTrade.symbolId] || "";
        rawTrade.side = rawTrade.isBuy ? "buy" : "sell";
        this.onTrade(rawCtx, rawTrade);
    }

    _dispatchBook(rawCtx, rawBook) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        rawBook.symbol = this._reverseMap[rawBook.symbolId] || "";
        this.onBookUpdate(rawCtx, rawBook);
    }

    _dispatchTouch(rawCtx, rawBook) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        rawBook.symbol = this._reverseMap[rawBook.symbolId] || "";
        this.onTouch(rawCtx, rawBook);
    }

    _dispatchBar(rawCtx, rawBar) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        rawBar.symbol = this._reverseMap[rawBar.symbolId] || "";
        this.onBar(rawCtx, rawBar);
    }

    _dispatchFill(rawCtx, rawEv) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        this.onFill(rawCtx, rawEv);
    }

    _dispatchOrderUpdate(rawCtx, rawEv) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        this.onOrderUpdate(rawCtx, rawEv);
    }

    _dispatchQueuePositionChange(rawCtx, rawEv) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        this.onQueuePositionChange(rawCtx, rawEv);
    }

    _dispatchMarketPositionChange(rawCtx, rawEv) {
        rawCtx.symbol = this._reverseMap[rawCtx.symbolId] || "";
        this.onMarketPositionChange(rawCtx, rawEv);
    }
}
