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
// more than 4096 live weak maps: each keeps its entries while their keys live
var wms = [], wkeys = [];
for (var w = 0; w < 5000; w++) { var wk = {}; var wmap = new WeakMap(); wmap.set(wk, { v: w }); wms.push(wmap); wkeys.push(wk); }
for (var r2 = 0; r2 < 30; r2++) { var junk = []; for (var j2 = 0; j2 < 5000; j2++) junk.push({ a: j2, b: 'x' + j2 }); }
var wbad = 0; for (var w = 0; w < 5000; w += 7) if (!wms[w].has(wkeys[w]) || wms[w].get(wkeys[w]).v !== w) wbad++;
eq(wbad, 0, 'many weak maps keep their entries');
// a big array made with new Array(n) is collected safely before it is filled
var holes = []; for (var h2 = 0; h2 < 20; h2++) { var ha = new Array(5000); ha[4999] = { h2: h2 }; holes.push(ha); }
for (var r3 = 0; r3 < 10; r3++) { var junk2 = []; for (var j3 = 0; j3 < 5000; j3++) junk2.push({ c: j3 }); }
eq(holes[19][4999].h2, 19, 'sparse new Array(n) survived GC');
print('gc done');
