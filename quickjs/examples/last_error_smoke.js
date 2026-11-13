// Smoke test: lrvx.lastError() says why a call that returns false or 0 failed.
//
// __lrvx_funding_schedule_load_tape, lrvx.recompressSegment,
// lrvx.extractSymbols and lrvx.extractTimeRange report failure through their
// return value, and 0 is also what an extract that matches nothing returns.
//   - each failure leaves its reason, naming the path and the C function

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

var root = '/tmp/lrvx-last-error-qjs-smoke-' + Date.now() + '-' + Math.floor(Math.random() * 1000000);
var missing = root + '/no_such_file';

var fs = __lrvx_funding_schedule_create();
check(__lrvx_funding_schedule_load_tape(fs, missing) === false,
      'load_tape returns false for a file that is not there');
check(lrvx.lastError().indexOf(missing) >= 0, 'and lastError() names it: ' + lrvx.lastError());
__lrvx_funding_schedule_destroy(fs);

check(lrvx.recompressSegment(missing, root + '/out.lrvx', 1) === false,
      'recompressSegment returns false');
check(lrvx.lastError().indexOf(missing) >= 0, 'and lastError() names the input: ' + lrvx.lastError());
check(lrvx.lastError().indexOf('lrvx_segment_recompress') >= 0, 'and the C function that failed');

check(lrvx.extractSymbols(missing, root + '/out.lrvx', [1]) === 0, 'extractSymbols returns 0');
check(lrvx.lastError().indexOf(missing) >= 0, 'and lastError() names the input');
check(lrvx.lastError().indexOf('lrvx_segment_extract_symbols') >= 0,
      'and the function is the one just called, not the one before');

check(lrvx.extractTimeRange(missing, root + '/out.lrvx', 0, 1000000000) === 0,
      'extractTimeRange returns 0');
check(lrvx.lastError().indexOf(missing) >= 0, 'and lastError() names the input');

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('last error smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
