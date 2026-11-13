'use strict';
/**
 * node/test/test_runner_on_order_event.js -- Runner.onOrderEvent: a venue's
 * order updates and fills reach a strategy's onFill / onOrderUpdate.
 *
 * The C API had the entry and Node had no wrapper, so a strategy on a live
 * Runner never heard of its orders and always read a position of 0. Status
 * names were also off by one row in Node: SUBMITTED read "ACCEPTED" and
 * ACCEPTED read "PENDING_NEW".
 *
 * Run from repo root:
 *   cd node && node test/test_runner_on_order_event.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function throwsWith(fn, type, needle, msg) {
  try {
    fn();
    check(false, `${msg} (did not throw)`);
  } catch (e) {
    check(e instanceof type && String(e.message).includes(needle), `${msg} (${e.message})`);
  }
}

function build(threaded) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const runner = new lrvx.Runner(reg, () => {}, threaded);
  const heard = [[], []];
  const ids = [0, 0];
  for (let i = 0; i < 2; i++) {
    runner.addStrategy({
      symbols: [sym],
      onTrade(_ctx, _trade, emit) {
        if (!ids[i]) { ids[i] = emit.limitBuy(100, 2); }
        heard[i].push({kind: 'trade'});
      },
      onFill(ctx, ev) {
        heard[i].push({kind: 'fill', status: ev.status, qty: ev.fillQty, position: ctx.position,
                       orderId: ev.orderId});
      },
      onOrderUpdate(ctx, ev) {
        heard[i].push({kind: 'update', status: ev.status, position: ctx.position,
                       reason: ev.rejectReason});
      },
    });
  }
  return {reg, sym, runner, heard, ids};
}

console.log('=== sync runner: events reach the strategy that issued the order ===');
{
  const {sym, runner, heard, ids} = build(false);
  check(typeof runner.onOrderEvent === 'function', 'Runner.onOrderEvent exists');
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000n);
  const second = ids[1];
  runner.onOrderEvent({orderId: second, symbol: sym, side: 'buy', status: 'SUBMITTED'});
  runner.onOrderEvent({orderId: second, symbol: sym, side: 'buy', status: 'ACCEPTED'});
  runner.onOrderEvent({orderId: second, symbol: sym, side: 'buy', status: 'PARTIALLY_FILLED',
                       fillQty: 0.5, fillPrice: 100, tsNs: 2000n, isMaker: true});
  runner.onOrderEvent({orderId: second, symbol: Number(sym), side: 'buy', status: 'FILLED',
                       orderType: 'limit', fillQty: 1.5, fillPrice: 100});
  const mine = heard[1].filter((e) => e.kind !== 'trade');
  check(mine.length === 4, `the issuing strategy heard four events (${mine.length})`);
  check(mine[0].kind === 'update' && mine[0].status === 'SUBMITTED',
        `a SUBMITTED event reads SUBMITTED (${mine[0] && mine[0].status})`);
  check(mine[1].kind === 'update' && mine[1].status === 'ACCEPTED',
        `an ACCEPTED event reads ACCEPTED (${mine[1] && mine[1].status})`);
  check(mine[2].kind === 'fill' && mine[2].position === 0.5,
        `ctx.position in onFill counts the fill (${mine[2] && mine[2].position})`);
  check(mine[3].kind === 'fill' && mine[3].position === 2, 'and the next one');
  check(heard[0].filter((e) => e.kind !== 'trade').length === 0,
        'the other strategy heard nothing and its position is its own');

  runner.onOrderEvent({orderId: ids[0], symbol: sym, side: 'buy', status: 'REJECTED',
                       rejectReason: 'insufficient margin'});
  const first = heard[0].filter((e) => e.kind !== 'trade');
  check(first.length === 1 && first[0].status === 'REJECTED' &&
        first[0].reason === 'insufficient margin' && first[0].position === 0,
        'a rejection carries its reason and the first strategy still reads 0');

  throwsWith(() => runner.onOrderEvent(), TypeError, 'options object', 'no argument');
  throwsWith(() => runner.onOrderEvent({symbol: sym, side: 'buy', status: 'FILLED'}),
             TypeError, 'orderId', 'a missing orderId');
  throwsWith(() => runner.onOrderEvent({orderId: second, symbol: sym, side: 'long',
                                        status: 'FILLED'}),
             TypeError, "'long'", 'an unknown side names the value');
  throwsWith(() => runner.onOrderEvent({orderId: second, symbol: sym, side: 'buy',
                                        status: 'DONE'}),
             TypeError, "'DONE'", 'an unknown status names the value');
  throwsWith(() => runner.onOrderEvent({orderId: second, symbol: sym, side: 'buy',
                                        status: 'FILLED', orderType: 'weird'}),
             TypeError, "'weird'", 'an unknown order type names the value');
  throwsWith(() => runner.onOrderEvent({orderId: second, symbol: sym, side: 'buy',
                                        status: 'FILLED', fillQty: '1'}),
             TypeError, 'fillQty', 'a string fillQty');
  throwsWith(() => runner.onOrderEvent({orderId: -1, symbol: sym, side: 'buy',
                                        status: 'FILLED'}),
             TypeError, 'orderId', 'a negative orderId');
  runner.stop();
}

console.log('=== a seeded position is what the strategy reads ===');
{
  const {sym, runner, heard, ids} = build(false);
  runner.seedPosition(1, sym, -1.5, 100);
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000n);
  runner.onOrderEvent({orderId: ids[1], symbol: sym, side: 'buy', status: 'FILLED',
                       fillQty: 2, fillPrice: 100});
  const fill = heard[1].find((e) => e.kind === 'fill');
  check(fill && Math.abs(fill.position - 0.5) < 1e-9,
        `the fill adds to the seeded short of 1.5 (${fill && fill.position})`);
  throwsWith(() => runner.seedPosition(1, sym, 1, 100), Error, 'already holds',
             'a second seed is refused');
  throwsWith(() => runner.seedPosition(7, sym, 1, 100), RangeError, 'strategyIndex 7',
             'an index the runner does not hold');
  throwsWith(() => runner.seedPosition(0, sym, '1'), TypeError, 'position',
             'a string position');
  runner.stop();
}

console.log('=== an order handed to an executor names its trailing callback rate ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'ETHUSDT', 0.01);
  const runner = new lrvx.Runner(reg, () => {}, false);
  const submitted = [];
  runner.setExecutor({submit(order) { submitted.push(order); }});
  runner.addStrategy({symbols: [sym], onTrade(_c, _t, emit) { emit.limitBuy(100, 1); }});
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000n);
  runner.stop();
  check(submitted.length === 1 && submitted[0].trailingBps === 0,
        `a limit order reads trailingBps 0 (${submitted[0] && submitted[0].trailingBps})`);
}

console.log('=== threaded runner: the event is queued behind the trades before it ===');
{
  const {sym, runner, heard, ids} = build(true);
  throwsWith(() => runner.onOrderEvent({orderId: 1, symbol: sym, side: 'buy', status: 'FILLED'}),
             Error, 'not started', 'a threaded runner that is not started refuses');
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000n);
  const deadline = Date.now() + 5000;
  const waitFor = (cond, then) => {
    if (cond() || Date.now() > deadline) { then(); } else { setTimeout(() => waitFor(cond, then), 5); }
  };
  waitFor(() => ids[0] !== 0, () => {
    for (let i = 0; i < 10; i++) { runner.onTrade(Number(sym), 100, 1, true, BigInt(2000 + i)); }
    runner.onOrderEvent({orderId: ids[0], symbol: sym, side: 'buy', status: 'FILLED',
                         fillQty: 2, fillPrice: 100});
    check(heard[0].every((e) => e.kind !== 'fill'), 'the call returns before the callback runs');
    waitFor(() => heard[0].some((e) => e.kind === 'fill'), () => {
      const kinds = heard[0].map((e) => e.kind);
      check(kinds.length === 12 && kinds[11] === 'fill',
            `the fill follows the eleven trades published before it (${kinds.join(',')})`);
      check(heard[0][11].position === 2, 'and its context counts it');
      check(heard[1].every((e) => e.kind === 'trade'), 'the other strategy heard no fill');
      runner.stop();
      console.log(`\n${passed} passed, ${failed} failed`);
      process.exit(failed === 0 ? 0 : 1);
    });
  });
}
