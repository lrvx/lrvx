// Smoke test: __lrvx_funding_schedule_tick returns the settlements it found.
//
// The binding asked the C API for a count and then for the events. The count
// call settled them, so the second call found nothing and every event came
// back as zeros.
//   - one tick returns each payment with its rate, mark, position and amount
//   - timestampNs is a BigInt holding the exact nanosecond
//   - a second tick at the same instant returns nothing
//
// The FUNDING lines are the payments with every float printed as its IEEE-754
// bits: python/tests/test_funding_tick_across_bindings.py reads them and
// compares them with what the Python binding returns for the same schedule.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}
function bits(x) {
    return new BigInt64Array(new Float64Array([x]).buffer)[0].toString();
}

var T1 = 1700000000123456789n;
var T2 = 1700000028800000001n;

var fs = __lrvx_funding_schedule_create();
__lrvx_funding_schedule_set_tape_by_symbol(fs, [T1, T1, T2], [1, 2, 0], [0.0001, -0.0002, 0.0003]);
var events = __lrvx_funding_schedule_tick(fs, T2, [1, 2], [2.0, -3.0], [50000.0, 2000.0]);

for (var i = 0; i < events.length; i++) {
    var e = events[i];
    console.log('FUNDING ' + e.timestampNs.toString() + ' ' + e.symbol + ' ' + bits(e.rate) + ' ' +
                bits(e.markPrice) + ' ' + bits(e.positionSigned) + ' ' + bits(e.amount));
}

check(events.length === 4, 'one tick returns the four payments due');
if (events.length === 4) {
    check(typeof events[0].timestampNs === 'bigint', 'timestampNs is a BigInt');
    check(events[0].timestampNs === T1, 'the first timestamp is exact');
    check(events[3].timestampNs === T2, 'the last timestamp is exact');
    check(events[0].symbol === 1 && events[1].symbol === 2, 'symbols in the order given');
    check(events[0].rate === 0.0001, 'the rate of symbol 1 at the first settlement');
    check(events[1].rate === -0.0002, 'the rate of symbol 2 at the first settlement');
    check(events[2].rate === 0.0003 && events[3].rate === 0.0003, 'the wildcard rate at the second');
    check(events[0].markPrice === 50000.0 && events[0].positionSigned === 2.0,
          'mark and position are the ones passed in');
    check(events[0].amount === -2.0 * 50000.0 * 0.0001, 'a long pays a positive rate');
    check(events[1].amount === 3.0 * 2000.0 * -0.0002, 'a short pays a negative rate');
}
check(__lrvx_funding_schedule_tick(fs, T2, [1, 2], [2.0, -3.0], [50000.0, 2000.0]).length === 0,
      'the same instant again settles nothing');
__lrvx_funding_schedule_destroy(fs);

// More payments than the first call has room for.
var HOUR = 3600n * 1000000000n;
var many = __lrvx_funding_schedule_create();
__lrvx_funding_schedule_set_constant(many, HOUR, 0.001);
var first = __lrvx_funding_schedule_tick(many, 1000n * HOUR, [1], [1.0], [100.0]);
check(first.length === 1, 'the opening tick settles the boundary just passed');
var burst = __lrvx_funding_schedule_tick(many, 1040n * HOUR, [1], [1.0], [100.0]);
check(burst.length === 40, 'forty boundaries in one tick are all returned');
if (burst.length === 40) {
    check(burst[0].timestampNs === 1001n * HOUR, 'starting after the last one settled');
    check(burst[39].timestampNs === 1040n * HOUR, 'and ending at now');
    check(burst[39].amount === -0.1, 'each with its amount');
}
__lrvx_funding_schedule_destroy(many);

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('funding schedule smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
