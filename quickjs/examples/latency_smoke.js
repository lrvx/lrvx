// Smoke test for the latency models exposed on the lrvx global.
// Runs under the lrvx QuickJS host. Asserts construction, sampling,
// reproducibility under reset(seed), and validation of bad inputs.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// ── ConstantLatency ────────────────────────────────────────────────
{
    var c = new lrvx.ConstantLatency({ feedNs: 100, orderNs: 200, fillNs: 300 });
    check(c.feedDelay() === 100, 'constant feed_delay returns configured value');
    check(c.orderDelay() === 200, 'constant order_delay returns configured value');
    check(c.fillDelay() === 300, 'constant fill_delay returns configured value');
    var s = c.sample();
    check(s.feedNs === 100 && s.orderNs === 200 && s.fillNs === 300,
          'sample() returns LatencySample with all three');
    c.destroy();
}

// ── GaussianLatency reproducibility ────────────────────────────────
{
    var g = new lrvx.GaussianLatency({
        feedMeanNs: 1000, feedStddevNs: 200, seed: 42,
    });
    var a = g.feedDelay();
    g.reset(42);
    var b = g.feedDelay();
    check(a === b, 'gaussian sequence is reproducible under reset(seed)');
    var c = new lrvx.GaussianLatency({ feedMeanNs: 1500, feedStddevNs: 0, seed: 0 });
    check(c.feedDelay() === 1500, 'stddev=0 collapses to mean');
    g.destroy();
    c.destroy();
}

// ── ExponentialLatency non-negativity ──────────────────────────────
{
    var e = new lrvx.ExponentialLatency({ feedMeanNs: 500, seed: 7 });
    var allNonNeg = true;
    for (var i = 0; i < 50; i++) {
        if (e.feedDelay() < 0) { allNonNeg = false; break; }
    }
    check(allNonNeg, 'exponential samples are non-negative');
    e.destroy();
}

// ── EmpiricalLatency draws subset ──────────────────────────────────
{
    var emp = new lrvx.EmpiricalLatency({ feedSamples: [10, 20, 30], seed: 0 });
    var allowed = { 10: 1, 20: 1, 30: 1 };
    var allInSet = true;
    for (var i = 0; i < 30; i++) {
        if (!allowed[emp.feedDelay()]) { allInSet = false; break; }
    }
    check(allInSet, 'empirical draws are subset of input samples');
    check(emp.orderDelay() === 0, 'empirical with no order_samples returns 0');
    emp.destroy();
}

// ── Validation ─────────────────────────────────────────────────────
{
    var threw = false;
    try { new lrvx.ConstantLatency({ feedNs: -1 }); }
    catch (e) { threw = true; }
    check(threw, 'negative feed_ns rejected');
    var threw2 = false;
    try { new lrvx.EmpiricalLatency({}); }
    catch (e) { threw2 = true; }
    check(threw2, 'all-empty empirical rejected');
}

// ── Slippage model names ───────────────────────────────────────────
// An unknown model name throws, listing the valid ones, instead of
// silently reading as "none".
{
    var ex = new SimulatedExecutor();
    var msg = '';
    try { ex.setDefaultSlippage('fixed_tick', 1, 0.01); }
    catch (e) { msg = String(e.message); }
    check(msg.indexOf("'fixed_ticks'") >= 0 && msg.indexOf("'volume_impact'") >= 0 &&
          msg.indexOf("'fixed_tick'") >= 0,
          'setDefaultSlippage rejects an unknown model: ' + msg);
    msg = '';
    try { ex.setSymbolSlippage(1, 'toString'); }
    catch (e) { msg = String(e.message); }
    check(msg.indexOf("'none'") >= 0, 'setSymbolSlippage rejects an unknown model: ' + msg);
    var ok = true;
    try { ex.setDefaultSlippage('fixed_bps', 0, 0, 5); ex.setSymbolSlippage(1, 'none'); }
    catch (e) { ok = false; }
    check(ok, 'valid slippage model names are accepted');
    ex.destroy();
}

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('latency smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
