// gc.js - allocation churn: closures, ropes, maps, cycles, typed arrays
var keep = [];
for (var round = 0; round < 40; round++) {
  var objs = [];
  for (var i = 0; i < 5000; i++) objs.push({ i: i, s: 'x' + i, f: (function (k) { return () => k; })(i), a: [i, i + 1] });
  var m = new Map(); for (var j = 0; j < 2000; j++) m.set('k' + j, objs[j]);
  var str = ''; for (var k = 0; k < 2000; k++) str += 'ab';
  var cyc = {}; cyc.self = cyc;
  keep.push(objs[round].f, str.length, m.get('k7').s);
  new Uint8Array(10000);
}
eq(keep.length, 120, 'kept');
eq(keep[0](), 0, 'closure survived GC'); eq(keep[3](), 1, 'closure 2');
eq(keep[1], 4000, 'rope length'); eq(keep[2], 'x7', 'map value survived');
var ws = new WeakMap(); (function () { for (var i = 0; i < 1000; i++) ws.set({}, i); })();
var big = []; for (var b = 0; b < 200000; b++) big.push(b * 2); eq(big[199999], 399998, 'big array');
var s2 = Array.from({ length: 1000 }, (_, i) => String.fromCharCode(65 + i % 26)).join(''); eq(s2.length, 1000, 'join big');
print('gc done');
