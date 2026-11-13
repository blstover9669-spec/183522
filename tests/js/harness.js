// harness.js - assertion helpers for the NovaJS host tests (loaded before each test file)
var __fails = 0, __passes = 0;
function __show(v) {
  try {
    if (typeof v === 'string') return JSON.stringify(v);
    if (typeof v === 'object' && v !== null) return JSON.stringify(v);
    return String(v);
  } catch (e) { return '<' + typeof v + '>'; }
}
function check(cond, msg) {
  if (cond) { __passes++; return; }
  __fails++;
  print('  FAILED: ' + msg);
}
function eq(a, b, msg) {
  var same = a === b || (a !== a && b !== b);
  if (same && a === 0 && b === 0 && 1 / a !== 1 / b) same = false;
  check(same, (msg || 'eq') + ': got ' + __show(a) + ', expected ' + __show(b));
}
function deq(a, b, msg) {
  var x = JSON.stringify(a), y = JSON.stringify(b);
  check(x === y, (msg || 'deq') + ': got ' + x + ', expected ' + y);
}
function throws(fn, type, msg) {
  try { fn(); } catch (e) {
    check(!type || e instanceof type, (msg || 'throws') + ': wrong error ' + e);
    return;
  }
  check(false, (msg || 'throws') + ': did not throw');
}
