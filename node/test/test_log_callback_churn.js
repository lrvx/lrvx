'use strict';
/**
 * node/test/test_log_callback_churn.js
 *
 * One thread logs without pause while another installs and detaches its log
 * callback over and over. Detaching the last callback removes the bridge from
 * the C logger, and that waits for the lines already inside the bridge; the
 * bridge's lines wait for the list of callbacks. Detaching used to hold that
 * list while it waited, so a line in the bridge and a detach on another
 * thread waited for each other forever.
 *
 * Run from repo root:
 *   cd node && node test/test_log_callback_churn.js
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
const lrvx = require(path.join(__dirname, '..'));

const SEC = 1_000_000_000n;
const BASE = 1_700_000_000n * SEC;

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

if (!isMainThread) {
  // The churning thread: install and detach as fast as it can.
  for (let i = 0; i < workerData.rounds; ++i) {
    lrvx.setLogCallback(() => {});
    lrvx.setLogCallback(null);
  }
  parentPort.postMessage('done');
  return;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-logchurn-'));
writeLaggingTape(dir);

// The logging thread: a worker of its own, so the main thread stays free to
// notice a hang.
const loggerSrc = `
  const { parentPort, workerData } = require('worker_threads');
  const lrvx = require(workerData.addon);
  const SEC = 1_000_000_000n;
  let stop = false;
  parentPort.on('message', () => { stop = true; });
  (function loop() {
    for (let i = 0; i < 20 && !stop; ++i) {
      new lrvx.DataReader(workerData.dir, undefined, undefined, { reorderWindowNs: SEC })
        .run([new lrvx.OHLCBinAggregator(60_000_000_000n, false)], 1);
    }
    if (!stop) setImmediate(loop); else parentPort.close();
  })();
`;
const addon = path.join(__dirname, '..');
const logger = new Worker(loggerSrc, { eval: true, workerData: { dir, addon } });
const churn = new Worker(__filename, { workerData: { rounds: 3000 } });

let finished = false;
const timer = setTimeout(() => {
  if (!finished) {
    console.error('  FAIL  installing and detaching while another thread logs hung');
    process.exit(1);
  }
}, 30000);

churn.once('message', async () => {
  finished = true;
  clearTimeout(timer);
  logger.postMessage('stop');
  await new Promise((resolve) => logger.once('exit', resolve));
  fs.rmSync(dir, { recursive: true, force: true });
  console.log('  ok  3000 installs and detaches while another thread logs');
  console.log('\n1 passed, 0 failed');
  process.exit(0);
});
churn.once('error', (e) => { console.error(e); process.exit(1); });
