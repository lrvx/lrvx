'use strict';
/**
 * node/test/test_grid_search_factory.js
 *
 * GridSearch reads the stats its factory returns. A field of the wrong type
 * read as 0 without a word, a return value that was not an object gave a row
 * of zeros, and an error the factory threw was swallowed by the C API so the
 * run handed back zeroed stats from that combination on. Now a wrong type or
 * a non-object is a TypeError naming the field, and whatever the factory
 * threw is what run() throws, class and all. Missing fields still read 0.
 *
 * Run from repo root:
 *   cd node && node test/test_grid_search_factory.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function grid(factory) {
  const gs = new lrvx.GridSearch();
  gs.addAxis([1, 2, 3]);
  gs.setFactory(factory);
  return gs;
}
function runThrows(factory, test, msg) {
  try {
    const r = grid(factory).run();
    check(false, `${msg}: run returned ${JSON.stringify(r.map((x) => x.stats.returnPct))}`);
  } catch (e) {
    check(test(e), `${msg} (${e.constructor.name}: ${e.message})`);
  }
}

console.log('=== a field of the wrong type ===');
runThrows(() => ({ returnPct: '1.5' }),
          (e) => e instanceof TypeError && /setFactory: the stats' returnPct must be a number/.test(e.message),
          'a string returnPct is refused');
runThrows(() => ({ totalTrades: 3n }),
          (e) => e instanceof TypeError && /totalTrades must be a number/.test(e.message),
          'a BigInt totalTrades is refused');
runThrows(() => ({ startTimeNs: 'yesterday' }),
          (e) => e instanceof TypeError && /startTimeNs/.test(e.message),
          'a string startTimeNs is refused');
runThrows(() => 42,
          (e) => e instanceof TypeError && /setFactory: the stats the factory returns must be an object/.test(e.message),
          'a number in place of the stats is refused');

console.log('=== an error the factory throws ===');
class SweepError extends Error {}
let calls = 0;
runThrows((params) => {
  calls++;
  if (params[0] === 2) throw new SweepError('bad combination');
  return { returnPct: params[0] };
}, (e) => e instanceof SweepError && e.message === 'bad combination',
'run throws what the factory threw');
check(calls === 2, `the run stops at the throwing combination (factory called ${calls} times)`);

console.log('=== what still works ===');
{
  const r = grid((params) => ({ returnPct: params[0] * 2, calmarRatio: 0.5, startTimeNs: 7n }))
    .run();
  check(r.length === 3 && r.map((x) => x.stats.returnPct).join() === '2,4,6',
        'numbers read through');
  check(r[0].stats.totalTrades === 0 && r[0].stats.sharpeRatio === 0,
        'fields left out read 0');
  check(r[0].stats.calmarRatio === 0.5, 'calmarRatio reads through');
  check(r[0].stats.startTimeNs === 7n && r[0].stats.endTimeNs === 0n,
        'the time bounds read through, and 0n when left out');
  const u = grid(() => ({ returnPct: undefined, totalTrades: 1 })).run();
  check(u[0].stats.returnPct === 0 && u[0].stats.totalTrades === 1,
        'a field set to undefined reads 0');
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
