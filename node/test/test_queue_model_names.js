// node/test/test_queue_model_names.js
//
// SimulatedExecutor.setQueueModel refuses a model it does not know -- by name
// or by code -- with the list of valid names, instead of reading it as "none".

'use strict';

const lrvx = require('..');

let _failed = 0;
let _passed = 0;
function check(cond, msg)
{
  if (cond)
  {
    _passed++;
    console.log(`  ok  - ${msg}`);
  }
  else
  {
    _failed++;
    console.log(`  FAIL - ${msg}`);
  }
}

function message(fn)
{
  try
  {
    fn();
  }
  catch (e)
  {
    return String(e.message);
  }
  return '';
}

const NAMES = ['none', 'tob', 'full', 'pro_rata', 'pro_rata_with_fifo', 'top_pro_lmm',
               'pro_rata_with_priority'];

const ex = new lrvx.SimulatedExecutor();
for (const bad of ['bogus', 'FULL', 'pro-rata'])
{
  const msg = message(() => ex.setQueueModel(bad, 1));
  check(msg.includes(`'${bad}'`) && NAMES.every((n) => msg.includes(`'${n}'`)),
        `setQueueModel('${bad}') throws with the valid names: ${msg}`);
}
const codeMsg = message(() => ex.setQueueModel(99, 1));
check(codeMsg.includes('99') && codeMsg.includes('pro_rata_with_priority'),
      `setQueueModel(99) throws: ${codeMsg}`);
for (const name of NAMES)
{
  check(message(() => ex.setQueueModel(name, 1)) === '', `setQueueModel('${name}') is accepted`);
}
check(message(() => ex.setQueueModel(6, 1)) === '', 'the last valid code is accepted');

console.log(`\n${_passed} passed, ${_failed} failed`);
process.exit(_failed === 0 ? 0 : 1);
