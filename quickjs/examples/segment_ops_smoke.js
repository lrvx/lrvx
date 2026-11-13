// Smoke test: segment writing, merging and validation.
//
//   - new DataWriter(dir, 0, 0) passed a 0 MB segment limit, so the writer
//     rolled to a new segment before every event and left an empty first one;
//     0 means the 256 MB default, as leaving the argument out does
//   - validateSegmentFull(path) with the flags left out turned both checks
//     off; a flag left out is on, and one that is given is honoured
//   - mergeDir returns outputPath and errors beside its counts

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

var root = '/tmp/lrvx-segment-ops-qjs-smoke-' + Date.now() + '-' + Math.floor(Math.random() * 1000000);
var SEC = 1000000000n;

function writeTape(dir, maxSegmentSize, stamps) {
    var w = maxSegmentSize === undefined ? new DataWriter(dir) : new DataWriter(dir, maxSegmentSize, 0);
    for (var i = 0; i < stamps.length; i++) {
        var ts = stamps[i] * SEC;
        w.writeTrade(ts, ts, 100.0 + i, 1.0, i + 1, 1, true);
    }
    w.destroy();
}

// ---- DataWriter: 0 is the default segment size ----
var zero = root + '/zero';
writeTape(zero, 0, [10n, 11n, 12n]);
var dz = lrvx.validateDataset(zero);
check(dz.totalSegments === 1, 'DataWriter(dir, 0, 0) writes one segment (got ' + dz.totalSegments + ')');
check(dz.totalEvents === 3, 'holding the three events (got ' + dz.totalEvents + ')');

var omitted = root + '/omitted';
writeTape(omitted, undefined, [10n, 11n, 12n]);
check(lrvx.validateDataset(omitted).totalSegments === 1, 'as new DataWriter(dir) does');

// ---- mergeDir: where it wrote, and what went wrong ----
var merged = lrvx.mergeDir(zero, root + '/merged');
check(merged.success === true && merged.segmentsMerged === 1 && merged.eventsWritten === 3,
      'mergeDir counts are what they were');
check(typeof merged.outputPath === 'string' && merged.outputPath.indexOf(root + '/merged') === 0,
      'outputPath is the segment written: ' + merged.outputPath);
check(Array.isArray(merged.errors) && merged.errors.length === 0, 'errors is an empty array');
check(lrvx.validateSegment(merged.outputPath) === true, 'and the segment at outputPath is valid');

var none = lrvx.mergeDir(root + '/no_such_dir', root + '/merged_none');
check(none.success === false && none.outputPath === '', 'nothing to merge: no outputPath');
check(none.errors.length >= 1 && typeof none.errors[0] === 'string',
      'and the reason is in errors: ' + none.errors.join(' | '));

// ---- validateSegmentFull: a flag left out is on ----
// Timestamps that go backwards are an anomaly the timestamp check counts.
var backwards = root + '/backwards';
writeTape(backwards, 16, [30n, 10n, 20n]);
var segment = lrvx.mergeDir(backwards, root + '/backwards_merged').outputPath;

var both = lrvx.validateSegmentFull(segment);
check(both.actualEventCount === 3, 'validateSegmentFull walks the three events');
check(both.timestampAnomalies > 0,
      'flags left out: timestamps are checked (anomalies ' + both.timestampAnomalies + ')');
check(lrvx.validateSegmentFull(segment, true, true).timestampAnomalies === both.timestampAnomalies,
      'true, true is the same');
check(lrvx.validateSegmentFull(segment, true, false).timestampAnomalies === 0,
      'verifyTimestamps false turns the check off');
check(lrvx.validateSegmentFull(segment, true).timestampAnomalies === both.timestampAnomalies,
      'one flag given, the other left out: still on');
check(lrvx.validateSegmentFull(segment, false, false).valid === true,
      'with both off the segment is only walked');

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('segment ops smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
