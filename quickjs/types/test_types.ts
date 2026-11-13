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
