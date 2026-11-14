// basic.js - core language: values, operators, control flow, functions, closures
eq(1 + 2, 3, 'add');
eq('a' + 1, 'a1', 'concat');
eq(7 % 3, 1, 'mod');
eq(2 ** 10, 1024, 'pow');
eq(-7 >> 1, -4, 'sar');
eq(-1 >>> 28, 15, 'shr');
eq(typeof null, 'object', 'typeof null');
eq(typeof undefined, 'undefined', 'typeof undefined');
eq(typeof function(){}, 'function', 'typeof fn');
eq(0.1 + 0.2, 0.30000000000000004, 'float');
eq(String(0.1 + 0.2), '0.30000000000000004', 'float str');
eq(1 / 3 + '', '0.3333333333333333', 'third');
eq(1e21 + '', '1e+21', '1e21');
eq(123456789 * 1000 + '', '123456789000', 'int');
eq((-0).toString(), '0', '-0');
var x = 10;
function add(a, b) { return a + b; }
eq(add(x, 5), 15, 'call');
let s = 0;
for (let i = 0; i < 10; i++) s += i;
eq(s, 45, 'for let');
var fns = [];
for (let i = 0; i < 3; i++) fns.push(() => i);
eq(fns.map(f => f()).join(','), '0,1,2', 'per-iteration let');
function counter() { let c = 0; return { inc: () => ++c, get: () => c }; }
var c1 = counter(); c1.inc(); c1.inc();
eq(c1.get(), 2, 'closure');
eq((function () { return typeof arguments; })(), 'object', 'arguments');
eq((function (a, b) { return arguments.length; })(1, 2, 3), 3, 'arguments.length');
var o = { a: 1, b: { c: [1, 2, 3] } };
eq(o.b.c[2], 3, 'member');
eq(o?.b?.c?.length, 3, 'optional chain');
eq(o?.z?.c, undefined, 'optional chain undefined');
eq(null ?? 'd', 'd', 'nullish');
eq(0 ?? 'd', 0, 'nullish 0');
var t = 0;
switch (3) { case 1: t = 1; break; case 3: t = 3; case 4: t += 1; break; default: t = 9; }
eq(t, 4, 'switch fallthrough');
outer: for (var i = 0; i < 5; i++) { for (var j = 0; j < 5; j++) { if (j == 2) continue outer; if (i == 3) break outer; } }
eq(i, 3, 'labels');
try { throw new Error('boom'); } catch (e) { eq(e.message, 'boom', 'catch'); } finally { t = 'f'; }
eq(t, 'f', 'finally');
function fin() { try { return 1; } finally { t = 'g'; } }
eq(fin(), 1, 'return through finally');
eq(t, 'g', 'finally ran');
eq(`a${1 + 1}b`, 'a2b', 'template');
eq([1, 2, 3].indexOf(2), 1, 'indexOf');
var keys = []; for (var k in { p: 1, q: 2 }) keys.push(k);
eq(keys.join(), 'p,q', 'for-in');
var vals = []; for (var v of [4, 5, 6]) vals.push(v);
eq(vals.join(), '4,5,6', 'for-of');
eq('abc'.toUpperCase(), 'ABC', 'toUpperCase');
eq([3, 1, 2].sort().join(), '1,2,3', 'sort');
eq(JSON.stringify({ a: [1, 'x', null], b: true }), '{"a":[1,"x",null],"b":true}', 'json');
eq(JSON.parse('{"x":[1,2,{"y":"z"}]}').x[2].y, 'z', 'json parse');
eq(parseInt('42px'), 42, 'parseInt');
eq(parseFloat('3.25e2x'), 325, 'parseFloat');
eq(Number('0x1F'), 31, 'Number hex');
eq(isNaN('abc'), true, 'isNaN');
eq((255).toString(16), 'ff', 'toString 16');
eq((3.14159).toFixed(2), '3.14', 'toFixed');
eq((1.005).toFixed(2), '1.00', 'toFixed exact');
eq((123.456).toPrecision(4), '123.5', 'toPrecision');
eq((0.000123).toExponential(1), '1.2e-4', 'toExponential');
eq(Math.max(1, 5, 3), 5, 'max');
eq([1, [2, [3, [4]]]].flat(Infinity).join(), '1,2,3,4', 'flat');
print('basic done');
/* a function with many nested functions (unique constants) and then many shared ones:
   the compiler's constant table once hung on this (chatgpt.com) */
(function () {
  var src = 'var fs = [];';
  for (var i = 0; i < 300; i++) src += 'fs.push(function () { return ' + i + '; });';
  for (var j = 0; j < 300; j++) src += 'fs.push("s' + j + '");';
  src += 'return fs.length + fs[299]() + fs[300].length;';
  eq(new Function(src)(), 600 + 299 + 2, 'many nested functions and constants');
})();
/* assigning a property found on the prototype makes an own property (core-js relies on it) */
(function () {
  var p = { x: 1, constructor: 'P' };
  var o = Object.create(p);
  o.x = 2;
  o.constructor = {};
  eq(p.x, 1, 'prototype property kept');
  eq(o.x, 2, 'own property made');
  eq(p.constructor, 'P', 'prototype constructor kept');
  var n = new Promise(function (r) { r(1); });
  n.constructor = {};
  eq(Promise.prototype.constructor, Promise, 'Promise.prototype.constructor kept');
  var q = Object.create(Object.create({ y: 1 }));
  q.y = 5;
  eq(Object.getPrototypeOf(Object.getPrototypeOf(q)).y, 1, 'two levels up kept');
  eq(Object.prototype.hasOwnProperty.call(q, 'y'), true, 'own after two levels');
})();
