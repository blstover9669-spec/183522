// errors.js - error types, TDZ, strict mode, stack traces
throws(() => undefinedVariable, ReferenceError, 'undeclared');
eq(typeof undeclaredThing, 'undefined', 'typeof undeclared');
throws(() => { tdz; let tdz = 1; }, ReferenceError, 'TDZ');
throws(() => { const k = 1; k = 2; }, TypeError, 'const assign');
throws(() => null.x, TypeError, 'null property');
throws(() => undefined.y(), TypeError, 'undefined call');
throws(() => ({}).nope(), TypeError, 'not a function');
throws(() => new (function () {}.bind())().x.y, TypeError, 'nested undefined');
throws(() => { 'use strict'; undeclaredAssign = 1; }, ReferenceError, 'strict undeclared assign');
(function () { sloppyGlobal = 5; })(); eq(sloppyGlobal, 5, 'sloppy implicit global');
throws(() => { 'use strict'; Object.freeze({ a: 1 }).a = 2; }, TypeError, 'strict frozen write');
throws(() => new Array(-1), RangeError, 'bad array length');
throws(() => (1).toFixed(200), RangeError, 'toFixed range');
throws(() => Symbol() + '', TypeError, 'symbol to string');
throws(() => { class A { constructor() { this.x; } } class B extends A { constructor() { this.y = 1; super(); } } new B(); }, ReferenceError, 'this before super');
try { null.foo; } catch (e) { check(/Cannot read properties of null/.test(e.message), 'message: ' + e.message); }
try { var q = {}; q.method(); } catch (e) { check(/method is not a function/.test(e.message), 'callee name in message: ' + e.message); }
function thrower() { throw new TypeError('inner'); }
try { thrower(); } catch (e) {
  eq(e.name, 'TypeError', 'name'); eq(e instanceof Error, true, 'subclass');
  check(e.stack.indexOf('thrower') >= 0, 'stack has function name: ' + e.stack);
  eq(String(e), 'TypeError: inner', 'toString');
}
var cause = new Error('outer', { cause: 'why' }); eq(cause.cause, 'why', 'cause');
var agg = new AggregateError([1, 2], 'many'); eq(agg.errors.length, 2, 'AggregateError');
class MyErr extends Error { constructor(m) { super(m); this.name = 'MyErr'; } }
try { throw new MyErr('custom'); } catch (e) { eq(e instanceof MyErr && e instanceof Error, true, 'custom error'); eq(e.message, 'custom', 'custom msg'); }
var fin = [];
function nested() { try { try { throw 1; } finally { fin.push('inner'); } } catch (e) { fin.push('caught' + e); } finally { fin.push('outer'); } }
nested(); eq(fin.join(), 'inner,caught1,outer', 'nested try/finally');
function brk() { for (var i = 0; i < 3; i++) { try { if (i == 1) break; } finally { fin.push('f' + i); } } return i; }
eq(brk(), 1, 'break through finally'); eq(fin.slice(-2).join(), 'f0,f1', 'finally on break');
var closed = false;
var iterable = { [Symbol.iterator]() { return { i: 0, next() { return { value: this.i++, done: false }; }, return() { closed = true; return {}; } }; } };
for (var v of iterable) { if (v == 2) break; } eq(closed, true, 'iterator closed on break');
closed = false; try { for (var v2 of iterable) throw 'x'; } catch (e) {} eq(closed, true, 'iterator closed on throw');
eq((() => { try { return 'try'; } finally { 'nothing'; } })(), 'try', 'finally without return');
eq((() => { try { return 'try'; } finally { return 'finally'; } })(), 'finally', 'finally overrides return');
throws(() => { throw 'plain'; }, null, 'throw non-error');
throws(() => JSON.parse(''), SyntaxError, 'empty JSON');
throws(() => eval('let let = 1'), SyntaxError, 'syntax error in eval');
throws(() => new Function('return )'), SyntaxError, 'syntax error in Function');
var deep = 0; function recurse() { deep++; recurse(); }
throws(recurse, RangeError, 'stack overflow'); check(deep > 1000, 'deep recursion count ' + deep);
print('errors done');
