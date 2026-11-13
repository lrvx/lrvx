// Smoke test: nanosecond clock readings are BigInt, and a BigInt is taken
// where a clock reading is an argument.
//
// __lrvx_account_mark_ts and validateDataset's firstTimestamp / lastTimestamp
// were Number, so a wall-clock nanosecond lost its last digits and could not
// be subtracted from the BigInt every other reading is. __lrvx_account_set_mark
// and __lrvx_liquidation_engine_on_marks ignored a BigInt timestamp.
//   - the mark timestamp reads back to the nanosecond, as a BigInt
//   - set_mark keeps a BigInt timestamp and a Number one
//   - validateDataset's range is BigInt and equals the tape's
//   - validateSegment / mergeSegments leave their reason in lastError()

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// 2^53 + 1 nanoseconds past a 2023 instant: not exact as a Number.
var ts = 1700000000123456789n;

var acct = __lrvx_account_create(1, 10000.0);
__lrvx_account_set_mark(acct, 7, 100.5, ts);
var read = __lrvx_account_mark_ts(acct, 7);
check(typeof read === 'bigint', 'mark_ts is a BigInt (got ' + typeof read + ')');
check(String(read) === '1700000000123456789', 'to the nanosecond: ' + String(read));
check(typeof read === 'bigint' && read - ts === 0n, 'and it subtracts from another reading');

__lrvx_account_set_mark(acct, 8, 100.5, 1700000000000);
check(String(__lrvx_account_mark_ts(acct, 8)) === '1700000000000', 'a Number timestamp is kept too');
__lrvx_account_destroy(acct);

var root = '/tmp/lrvx-clock-readings-qjs-smoke-' + Date.now() + '-' + Math.floor(Math.random() * 1000000);
var w = new DataWriter(root, 64, 0);
var first = 1700000000000000001n;
var last = 1700000005000000003n;
w.writeTrade(first, first, 100.0, 1.0, 1, 1, true);
w.writeTrade(last, last, 101.0, 1.0, 2, 1, false);
w.destroy();

var ds = lrvx.validateDataset(root);
check(ds.valid === true && ds.totalEvents === 2, 'validateDataset reads the two events');
check(typeof ds.firstTimestamp === 'bigint' && typeof ds.lastTimestamp === 'bigint',
      'firstTimestamp / lastTimestamp are BigInt (got ' + typeof ds.firstTimestamp + ')');
check(String(ds.firstTimestamp) === String(first), 'firstTimestamp is exact: ' + String(ds.firstTimestamp));
check(String(ds.lastTimestamp) === String(last), 'lastTimestamp is exact: ' + String(ds.lastTimestamp));

var missing = root + '/no_such_segment.lrvx';
check(lrvx.validateSegment(missing) === false, 'validateSegment returns false for a missing file');
check(lrvx.lastError().indexOf(missing) >= 0, 'and lastError() names it: ' + lrvx.lastError());
check(lrvx.mergeSegments(root + '/no_such_dir', root + '/merged') === false,
      'mergeSegments returns false when there is nothing to merge');
check(lrvx.lastError().indexOf('no_such_dir') >= 0, 'and lastError() says where: ' + lrvx.lastError());

var merged = lrvx.mergeDir(root, root + '/merged_dir');
check(merged.success === true && merged.segmentsMerged === 1 && merged.eventsWritten === 2,
      'mergeDir returns success, segmentsMerged, eventsWritten, bytesWritten');

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('clock readings smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
