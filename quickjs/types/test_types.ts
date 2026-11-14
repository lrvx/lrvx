// Type-checked, never run: `tsc --noEmit -p quickjs/types/tsconfig.json`
// (part of `npm run typecheck` in node/). It holds lrvx.d.ts to the calls
// quickjs/examples/run_trace_smoke.js makes.

const rec = new lrvx.TraceRecorder({
    path: '/tmp/run.lrvxrun',
    strategyId: 'types',
    strategyHash: 'sha256:test',
    runStartedNs: 1700000000000000000n,
});
rec.addTapeRef({ path: 'BTCUSDT.lrvx', contentHash: 'sha256:abc', firstEventNs: 1n, lastEventNs: 2 });
rec.writeSignal({ runTsNs: 1n, signalId: 42, name: 'x', symbolIds: [1, 2], payload: '{}' });
rec.writeSignal({ runTsNs: 2, signalId: (7n << 32n) + 5n });
rec.writeOrderEvent({ runTsNs: 3n, orderId: 7, parentSignalId: 42, eventKind: 1, reason: '' });
rec.writeFill({ runTsNs: 4n, orderId: 7, fillId: 1, priceRaw: 100, qtyRaw: 1, liquidity: 1 });
rec.setRunEndedNs(5n);
rec.close();
rec.destroy();

const reader = new lrvx.TraceReader('/tmp/run.lrvxrun');
const strategyId: string = reader.strategyId();
const started: bigint = reader.runStartedNs();
const ended: bigint = reader.runEndedNs();
for (const s of reader.readAllSignals()) {
    const ts: bigint = s.runTsNs;
    const low: number = s.signalId;
    const whole: bigint = s.signalId64;
    const symbols: number[] = s.symbolIds;
    const payload: string = s.payload;
    void [ts, low, whole, symbols, payload, s.feedTsNs, s.flags, s.strengthRaw, s.name];
}
for (const e of reader.readAllOrderEvents()) {
    const id: bigint = e.orderId;
    const parent: bigint = e.parentSignalId;
    const reason: string = e.reason;
    void [id, parent, reason, e.priceRaw, e.qtyRaw, e.symbolId, e.eventKind, e.side,
          e.orderType, e.flags, e.runTsNs, e.feedTsNs];
}
for (const f of reader.readAllFills()) {
    const id: bigint = f.fillId;
    const order: bigint = f.orderId;
    void [id, order, f.priceRaw, f.qtyRaw, f.feeRaw, f.symbolId, f.side, f.liquidity,
          f.runTsNs, f.feedTsNs];
}
reader.destroy();
void [strategyId, started, ended];

// @ts-expect-error a signal needs its run timestamp
rec.writeSignal({ signalId: 1 });
// @ts-expect-error the reader takes a path
new lrvx.TraceReader();

// The members of `lrvx` that had no typing.
const model = new lrvx.GaussianLatency({ feedMeanNs: 100, feedStddevNs: 5, seed: 1 });
const draw: number = model.sample().orderNs + model.feedDelay();
model.destroy();
new lrvx.ConstantLatency().destroy();
new lrvx.ExponentialLatency({ orderMeanNs: 10 }).destroy();
new lrvx.EmpiricalLatency({ feedSamples: [1, 2, 3] }).destroy();

const diff = lrvx.tapeDiff('/tmp/a', '/tmp/b', { maxMismatches: 4 });
const firstDiff: number | null = diff.firstDivergenceIndex;
const leftTs: bigint | undefined = diff.mismatches[0]?.left.exchangeTsNs;

const enc = new lrvx.DeltaBookEncoder({ anchorEvery: 10 });
const frame = enc.encode(1, [{ priceRaw: 1, qtyRaw: 2 }], []);
const rep = new lrvx.DeltaBookReplayer();
const book = rep.apply(frame.isDelta ? 1 : 0, 1, frame.bids, frame.asks);
enc.destroy();
rep.destroy();

const bars = new lrvx.BarDispatchRecorder();
const slot: number = bars.addTimeIntervalSeconds(60);
bars.onTrade(1, 100, 1, 1n);
bars.finalize();
const closed: number = bars.count() + bars.typeAt(0) + bars.paramAt(0);
bars.destroy();

const twap = new lrvx.TWAPExecutor({ targetQty: 1, side: 'sell', durationNs: 10n, sliceCount: 2,
                                     startTimeNs: 0 });
for (const child of twap.step(1n)) {
    const childTs: bigint = child.timestampNs;
    const kind: 'market' | 'limit' = child.type;
    void [childTs, kind, child.orderId, child.qty, child.price];
}
twap.reportFill(0.5);
twap.destroy();
new lrvx.VWAPExecutor({ targetQty: 1, volumeCurve: [[1000, 200]] }).destroy();
new lrvx.IcebergExecutor({ targetQty: 5, visibleQty: 1 }).destroy();
const pov = new lrvx.POVExecutor({ targetQty: 1, participationRate: 0.1 });
pov.observeVolume(10);
pov.destroy();

const risk = new lrvx.PortfolioRiskAggregator({ rules: { maxDailyLoss: 100 }, initialEquity: 1000 });
risk.update('a', { realizedPnl: -5, tradeCount: 1 });
const breach = risk.checkOrder('a', 10, 'buy');
const halted: boolean = risk.snapshot().killSwitchActive;
risk.destroy();

const graph = new lrvx.IndicatorGraph();
graph.setBars(1, [1, 2, 3]);
graph.addNode('twice', [], (g, sym) => Array.from(g.close(sym), (x) => 2 * x));
const twice = graph.require(1, 'twice');
graph.destroy();
const live = new lrvx.StreamingIndicatorGraph();
live.addNode('last', null, (g, sym) => g.close(sym)[0] ?? 0);
live.step(1, 100);
const now: number = live.current(1, 'last');
live.destroy();
const labels: number[] = lrvx.targets.future_return([1, 2, 3], 1);
void [draw, firstDiff, leftTs, book, slot, closed, breach, halted, twice, now, labels];

class Cancels extends Strategy {
    onStart(): void {
        this.cancel(7);
        this.cancel(7n);
        this.modify(7n, { price: 1, qty: 1 });
        // @ts-expect-error an order id is a number or a bigint
        this.cancel('7');
    }
}
void Cancels;

// @ts-expect-error an algo side is 'buy' or 'sell'
new lrvx.TWAPExecutor({ targetQty: 1, side: 'short', durationNs: 1, sliceCount: 1, startTimeNs: 0 });
// @ts-expect-error not a member of lrvx
lrvx.noSuchMember;

class PositionFacts extends Strategy {
    onBar(ctx: SymbolContext): void {
        const entry: number | null = this.entryPrice('BTCUSDT');
        const bars: number = this.barsInPosition('BTCUSDT');
        const primaryEntry: number | null = this.entryPrice();
        const primaryBars: number = this.barsInPosition();
        const fromCtx: boolean = this.entryPrice(ctx.symbol) === ctx.entryPrice
            && this.barsInPosition(ctx.symbol) === ctx.barsInPosition;
        // @ts-expect-error flat reads null, so the entry is not always a number
        const always: number = this.entryPrice();
        // @ts-expect-error the bar count is never null
        const noBars: null = this.barsInPosition();
        void [entry, bars, primaryEntry, primaryBars, fromCtx, always, noBars];
    }
}
void PositionFacts;

// Strategy._resolve takes a symbol name, a numeric symbol id (ctx.symbolId),
// or nothing for the primary symbol; every symbol argument accepts all three.
class SymbolArguments extends Strategy {
    onTrade(ctx: SymbolContext): void {
        const id: number = ctx.symbolId;
        for (const s of [id, 'BTCUSDT', undefined]) {
            const pos: number = this.position(s);
            const last: number = this.lastPrice(s);
            const bid: number | null = this.bestBid(s);
            const ask: number | null = this.bestAsk(s);
            const mid: number | null = this.midPrice(s);
            const entry: number | null = this.entryPrice(s);
            const held: number = this.barsInPosition(s);
            this.cancelAll(s);
            const closeId: number = this.closePosition(s);
            void [pos, last, bid, ask, mid, entry, held, closeId];
        }
        const byId: number[] = [
            this.position(id), this.lastPrice(id), this.closePosition(id),
            this.marketBuy({ symbol: id, qty: 1 }),
            this.marketSell({ symbol: id, qty: 1 }),
            this.limitBuy({ symbol: id, price: 1, qty: 1 }),
            this.limitSell({ symbol: id, price: 1, qty: 1, tif: 'IOC' }),
            this.stopMarket({ symbol: id, side: 'sell', trigger: 1, qty: 1 }),
            this.stopLimit({ symbol: id, side: 'sell', trigger: 1, price: 1, qty: 1 }),
            this.takeProfitMarket({ symbol: id, side: 'sell', trigger: 1, qty: 1 }),
            this.takeProfitLimit({ symbol: id, side: 'sell', trigger: 1, price: 1, qty: 1 }),
            this.trailingStop({ symbol: id, side: 'sell', offset: 1, qty: 1 }),
            this.trailingStopPercent({ symbol: id, side: 'sell', callbackBps: 50, qty: 1 }),
        ];
        const quotes: (number | null)[] = [
            this.bestBid(id), this.bestAsk(id), this.midPrice(id), this.entryPrice(id),
        ];
        this.cancelAll(id);
        this.cancelAll('BTCUSDT');
        this.cancelAll();
        this.marketBuy({ symbol: 'BTCUSDT', qty: 1 });
        this.marketBuy({ qty: 1 });
        // @ts-expect-error a symbol is a name or an id, not a boolean
        this.position(true);
        // @ts-expect-error a symbol is a name or an id, not a bigint
        this.marketBuy({ symbol: 1n, qty: 1 });
        void [byId, quotes];
    }
}
void SymbolArguments;

// The per-(symbol, barType, param) ring of closed bars. A bar is the LrvxBar
// shape node/index.d.ts calls ClosedBar: OHLCV as Numbers, startNs / endNs as
// BigInts, nothing else. `param` reaches the engine through JS_ToInt64Ext, so
// a Number and a BigInt both work. The ring is empty (null, []) until a bar
// of that timeframe closes; lastNClosedBars is oldest first.
class BarRing extends Strategy {
    onTrade(ctx: SymbolContext): void {
        const M5 = 5 * 60 * 1000000000;
        const last = this.lastClosedBar('BTCUSDT', 0, M5);
        // @ts-expect-error null until a bar of this timeframe has closed
        const unchecked: number = last.close;
        if (last !== null) {
            const open: number = last.open;
            const high: number = last.high;
            const low: number = last.low;
            const close: number = last.close;
            const volume: number = last.volume;
            const startNs: bigint = last.startNs;
            const endNs: bigint = last.endNs;
            // @ts-expect-error the times are BigInts, not Numbers
            const startAsNumber: number = last.startNs;
            // @ts-expect-error no `ts`: this is not the loadCsv Bar
            void last.ts;
            // @ts-expect-error no barType on a ring bar
            void last.barType;
            // @ts-expect-error no closeReason on a ring bar
            void last.closeReason;
            // @ts-expect-error no buyVolume on a ring bar
            void last.buyVolume;
            // @ts-expect-error camelCase, not start_ns
            void last.start_ns;
            void [open, high, low, close, volume, startNs, endNs, startAsNumber];
        }
        type RingBar = NonNullable<ReturnType<BarRing['lastClosedBar']>>;
        // Exactly these seven fields: a missing or an extra one fails here.
        const fields: Record<keyof RingBar, true> = {
            open: true, high: true, low: true, close: true, volume: true,
            startNs: true, endNs: true,
        };
        const byIdBigParam: RingBar | null = this.lastClosedBar(ctx.symbolId, 0, BigInt(M5));
        const primary: RingBar | null = this.lastClosedBar(undefined, 0, M5);

        const bars = this.lastNClosedBars('BTCUSDT', 0, M5, 10);
        const count: number = bars.length;
        const oldest: RingBar = bars[0];
        const same: RingBar[] = this.lastNClosedBars(ctx.symbolId, 0, BigInt(M5), 3);
        const fromPrimary: RingBar[] = this.lastNClosedBars(undefined, 0, M5, 3);
        // @ts-expect-error an empty ring is [], never null
        const nullable: null = this.lastNClosedBars('BTCUSDT', 0, M5, 1);

        const cap: number = this.barRingCapacity;
        const set: void = this.setBarRingCapacity(128);
        // @ts-expect-error setBarRingCapacity returns nothing
        const setResult: number = this.setBarRingCapacity(128);
        // @ts-expect-error read-only: setBarRingCapacity changes it
        this.barRingCapacity = 8;
        // @ts-expect-error barType is a Number (0 = Time), not a name
        this.lastClosedBar('BTCUSDT', 'time', M5);
        // @ts-expect-error barType is a Number (0 = Time), not a name
        this.lastNClosedBars('BTCUSDT', 'time', M5, 3);
        // @ts-expect-error param is a Number or a BigInt, not a timeframe string
        this.lastClosedBar('BTCUSDT', 0, '5m');
        // @ts-expect-error n is a count
        this.lastNClosedBars('BTCUSDT', 0, M5);
        // @ts-expect-error a BigInt capacity reads as 0 at run time and becomes 1
        this.setBarRingCapacity(64n);
        // @ts-expect-error a symbol is a name or an id, not a boolean
        this.lastClosedBar(true, 0, M5);
        void [fields, byIdBigParam, primary, count, oldest, same, fromPrimary, nullable, cap,
            set, setResult];
    }
}
void BarRing;
