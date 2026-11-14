// Threaded Runner lifetime: start opens the callback channel, stop releases it.
//
// Run:  node docs/examples/node_threaded_runner_lifetime.js
//
// Nothing at the bottom of this file forces the process to exit. It ends on
// its own because runner.stop() hands Node's event loop back. See
// docs/explanation/node-threaded-runner-lifetime.md.

const lrvx = require('../../node');

const reg = new lrvx.SymbolRegistry();
const sym = Number(reg.addSymbol('binance', 'BTCUSDT', 0.01));

let delivered = 0;

const runner = new lrvx.Runner(reg, () => {}, /* threaded */ true);
runner.addStrategy({
  symbols: [sym],
  onStart() { console.log('strategy started'); },
  onTrade(_ctx, trade) { delivered += 1; void trade.price; },
  onStop() { console.log(`strategy stopped after ${delivered} trades`); },
});

runner.start();
for (let i = 0; i < 1000; i++) {
  runner.onTrade(sym, 100 + (i % 10), 1, i % 2 === 0, 1_000_000 + i);
}

// Callbacks arrive on the JS thread at the next tick, so give the loop one
// before stopping. onStop is queued by stop() and still runs afterwards.
setTimeout(() => {
  runner.stop();
  setTimeout(() => {
    console.log(`delivered ${delivered} of 1000 published trades`);
  }, 100);
}, 500);
