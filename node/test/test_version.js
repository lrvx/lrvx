'use strict';
/**
 * node/test/test_version.js -- lrvx.version() is the release the addon was
 * built from: the library's lrvx_version(), equal to the version in
 * package.json (scripts/set-version.sh keeps that in step with
 * CMakeLists.txt and pyproject.toml). 0.12.0 exposed only the C ABI number.
 *
 * Run from repo root:
 *   cd node && node test/test_version.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));
const pkg = require(path.join(__dirname, '..', 'package.json'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

check(typeof lrvx.version === 'function', `lrvx.version is a function (${typeof lrvx.version})`);
let v = null;
try { v = lrvx.version(); } catch (e) { console.error(`  lrvx.version() threw: ${e.message}`); }
check(typeof v === 'string', `lrvx.version() returns a string (${typeof v})`);
check(v === pkg.version, `lrvx.version() === package.json version (${v} vs ${pkg.version})`);
check(/^\d+\.\d+\.\d+([.-][A-Za-z0-9.]+)?$/.test(String(v)), `release string shape (${v})`);
check(typeof v === 'string' && String(lrvx.capiAbiVersion()) !== v,
      'distinct from the C ABI version');

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
