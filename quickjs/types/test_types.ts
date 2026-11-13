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
    const whole: number = s.signalId64;
    const symbols: number[] = s.symbolIds;
    const payload: string = s.payload;
    void [ts, low, whole, symbols, payload, s.feedTsNs, s.flags, s.strengthRaw, s.name];
}
for (const e of reader.readAllOrderEvents()) {
    const id: number = e.orderId;
    const reason: string = e.reason;
    void [id, reason, e.parentSignalId, e.priceRaw, e.qtyRaw, e.symbolId, e.eventKind, e.side,
          e.orderType, e.flags, e.runTsNs, e.feedTsNs];
}
for (const f of reader.readAllFills()) {
    const id: number = f.fillId;
    void [id, f.orderId, f.priceRaw, f.qtyRaw, f.feeRaw, f.symbolId, f.side, f.liquidity,
          f.runTsNs, f.feedTsNs];
}
reader.destroy();
void [strategyId, started, ended];

// @ts-expect-error a signal needs its run timestamp
rec.writeSignal({ signalId: 1 });
// @ts-expect-error the reader takes a path
new lrvx.TraceReader();
