'use strict';
/**
 * node/test/test_log_callback_envs.js
 *
 * setLogCallback in one environment installs, replaces and removes only that
 * environment's callback. The callback used to be one process-wide static, so
 * a worker's setLogCallback replaced the main thread's function with one from
 * another isolate and its setLogCallback(null) detached the main thread's as
 * well. The engine logs from whatever thread it is on and cannot say which
 * environment a line belongs to, so every installed callback receives every
 * line, each on its own thread.
 *
 * A line is produced by reading a tape with an event past the reorder window,
 * which the reader reports through the logger.
 *
 * Run from repo root:
 *   cd node && node test/test_log_callback_envs.js
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
const lrvx = require(path.join(__dirname, '..'));

const SEC = 1_000_000_000n;
const BASE = 1_700_000_000n * SEC;
const MARK = 'past reorder window';

function writeLaggingTape(dir) {
  const w = new lrvx.DataWriter(dir, 4, 0);
  for (let i = 0; i < 10; ++i) {
    const ts = BASE + BigInt(i) * SEC;
    w.writeTrade(ts, ts, 100.0, 1.0, i, 1, 0);
  }
  const late = BASE - 30n * SEC;
  w.writeTrade(late, late, 100.0, 1.0, 999, 2, 0);
  w.close();
}

// One logged line: a run over the tape drops its late event and says so.
function logOnce(dir) {
  new lrvx.DataReader(dir, undefined, undefined, { reorderWindowNs: SEC })
    .run([new lrvx.OHLCBinAggregator(60_000_000_000n, false)], 1);
}

// Resolves with whether `count()` reached `n` within a second.
function reaches(count, n) {
  return new Promise((resolve) => {
    const t0 = Date.now();
    const poll = () => {
      if (count() >= n) return resolve(true);
      if (Date.now() - t0 > 1000) return resolve(false);
      setTimeout(poll, 5);
    };
    poll();
  });
}

if (!isMainThread) {
  let lines = 0;
  lrvx.setLogCallback((_lvl, msg) => { if (msg.includes(MARK)) lines++; });
  parentPort.on('message', async (msg) => {
    if (msg === 'log') {
      logOnce(workerData.dir);
      parentPort.postMessage({ got: await reaches(() => lines, 1) });
    } else if (msg === 'detach') {
      lrvx.setLogCallback(null);
      parentPort.postMessage({ detached: true });
    } else if (msg === 'exit') {
      parentPort.close();
    }
  });
  parentPort.postMessage({ ready: true });
  return;
}

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function next(worker) {
  return new Promise((resolve, reject) => {
    worker.once('message', resolve);
    worker.once('error', reject);
  });
}

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-logenv-'));
  try {
    writeLaggingTape(dir);
    let mainLines = 0;
    lrvx.setLogCallback((_lvl, msg) => { if (msg.includes(MARK)) mainLines++; });

    logOnce(dir);
    check(await reaches(() => mainLines, 1), 'the main thread receives a line it logged');

    const worker = new Worker(__filename, { workerData: { dir } });
    await next(worker);
    const before = mainLines;
    worker.postMessage('log');
    const w = await next(worker);
    check(w.got, 'the worker receives a line it logged');
    check(await reaches(() => mainLines, before + 1),
          'the main thread receives the worker\'s line as well');

    worker.postMessage('detach');
    await next(worker);
    const before2 = mainLines;
    logOnce(dir);
    check(await reaches(() => mainLines, before2 + 1),
          'the worker detaching leaves the main thread\'s callback installed');

    const again = new Worker(__filename, { workerData: { dir } });
    await next(again);
    await new Promise((resolve) => { again.once('exit', resolve); again.terminate(); });
    await new Promise((resolve) => { worker.once('exit', resolve); worker.postMessage('exit'); });
    const before3 = mainLines;
    logOnce(dir);
    check(await reaches(() => mainLines, before3 + 1),
          'a worker torn down with its callback installed leaves the main thread\'s working');

    lrvx.setLogCallback(null);
    const before4 = mainLines;
    logOnce(dir);
    check(!(await reaches(() => mainLines, before4 + 1)),
          'setLogCallback(null) detaches the main thread\'s callback');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }

  console.log(`\n${passed} passed, ${failed} failed`);
  process.exit(failed === 0 ? 0 : 1);
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
