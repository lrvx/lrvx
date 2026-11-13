'use strict';
/**
 * node/test/test_venue_stack_from_venue.js
 *
 * VenueStack.fromVenue takes the venue by name. A name that was not a string
 * was read as the empty string and reported as "unknown venue: ", which said
 * neither which argument was wrong nor that it had the wrong type. Now it is
 * a TypeError naming the parameter; an unknown string still names the venue.
 *
 * Run from repo root:
 *   cd node && node test/test_venue_stack_from_venue.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function threw(fn) {
  try { fn(); return null; } catch (e) { return e; }
}

for (const [what, name] of [['a number', 1], ['undefined', undefined], ['null', null],
                            ['an object', { venue: 'bybit' }]]) {
  const e = threw(() => lrvx.VenueStack.fromVenue(name, 1, 100000));
  check(e instanceof TypeError && e.message === 'fromVenue: name must be a string',
        `fromVenue refuses ${what} as the name (${e && `${e.constructor.name}: ${e.message}`})`);
}

{
  const e = threw(() => lrvx.VenueStack.fromVenue('nope', 1, 100000));
  check(e && /unknown venue: nope/.test(e.message), 'an unknown name still names the venue');
}
check(lrvx.VenueStack.fromVenue('okx', 1, 100000).venueName() === 'okx_swap',
      'a known name still builds its stack');

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
