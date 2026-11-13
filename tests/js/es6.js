// es6.js - classes, destructuring, spread/rest, defaults, symbols, iterators, generators
class Animal {
  #secret = 42;
  static count = 0;
  constructor(name) { this.name = name; Animal.count++; }
  speak() { return this.name + ' makes a sound'; }
  get upper() { return this.name.toUpperCase(); }
  set upper(v) { this.name = v.toLowerCase(); }
  get secret() { return this.#secret; }
  #hidden() { return 'h' + this.#secret; }
  callHidden() { return this.#hidden(); }
  static create(n) { return new this(n); }
  static { this.ready = true; }
}
class Dog extends Animal {
  constructor(name, breed) { super(name); this.breed = breed; }
  speak() { return super.speak() + ' (woof)'; }
  toString() { return `Dog(${this.name})`; }
}
var d = new Dog('rex', 'lab');
eq(d.speak(), 'rex makes a sound (woof)', 'super method');
eq(d.upper, 'REX', 'getter');
d.upper = 'MAX'; eq(d.name, 'max', 'setter');
eq(d.secret, 42, 'private field');
eq(d.callHidden(), 'h42', 'private method');
eq(Animal.count, 1, 'static field');
eq(Animal.ready, true, 'static block');
eq(Dog.create('a') instanceof Dog, true, 'static inherited + this');
eq(String(d), 'Dog(max)', 'toString override');
eq(d instanceof Animal, true, 'instanceof');
eq(Object.getPrototypeOf(Dog), Animal, 'class proto chain');
throws(() => Dog(), TypeError, 'class call without new');
class Counter { count = 0; inc = () => ++this.count; }
var cc = new Counter(); cc.inc(); cc.inc();
eq(cc.count, 2, 'field arrow this');
class Ext extends Array { sum() { return this.reduce((a, b) => a + b, 0); } }
var ex = Ext.from([1, 2, 3]);
eq(ex.sum(), 6, 'extends Array');
eq(ex instanceof Ext, true, 'Array subclass instance');
var anon = class {}; eq(anon.name, 'anon', 'class name inference');
eq((class Foo {}).name, 'Foo', 'class name');
// destructuring
var [a, , b = 5, ...rest] = [1, 2, undefined, 4, 5];
eq(a, 1, 'array destr'); eq(b, 5, 'default'); deq(rest, [4, 5], 'rest');
var { p, q: { r }, s = 's', ...others } = { p: 1, q: { r: 2 }, t: 3, u: 4 };
eq(p + r, 3, 'object destr'); eq(s, 's', 'obj default'); deq(others, { t: 3, u: 4 }, 'obj rest');
function f({ x = 1, y } = {}, [z] = [9]) { return x + y + z; }
eq(f({ y: 2 }), 12, 'param destr');
eq(f(), NaN, 'param destr undefined');
var sw1 = 1, sw2 = 2; [sw1, sw2] = [sw2, sw1];
eq(sw1 * 10 + sw2, 21, 'swap');
var key = 'dyn';
var o = { [key + 1]: 1, method() { return 'm'; }, get g() { return 'g'; }, ...{ sp: 1 }, key };
eq(o.dyn1, 1, 'computed key'); eq(o.method(), 'm', 'method'); eq(o.g, 'g', 'getter literal'); eq(o.sp, 1, 'spread prop'); eq(o.key, 'dyn', 'shorthand');
function sum(...n) { return n.reduce((a, b) => a + b, 0); }
eq(sum(...[1, 2], 3, ...new Set([4])), 10, 'spread call');
eq(Math.max(...[1, 9, 3]), 9, 'spread native');
deq([..."ab", ...[1]], ['a', 'b', 1], 'spread array');
function dflt(a, b = a + 1, c = () => a + b) { return c(); }
eq(dflt(1), 3, 'defaults referencing params');
// symbols
var sym = Symbol('desc');
eq(typeof sym, 'symbol', 'typeof symbol'); eq(sym.description, 'desc', 'description'); eq(sym.toString(), 'Symbol(desc)', 'sym toString');
var so = { [sym]: 1 }; eq(so[sym], 1, 'symbol key'); eq(Object.keys(so).length, 0, 'symbol not enumerated');
eq(Symbol.for('k'), Symbol.for('k'), 'Symbol.for');
// iterators
var it = { from: 1, to: 3, [Symbol.iterator]() { var c = this.from, t = this.to; return { next: () => c <= t ? { value: c++, done: false } : { value: undefined, done: true } }; } };
deq([...it], [1, 2, 3], 'custom iterator');
function* gen() { var x = yield 1; yield x * 2; return 'end'; }
var g = gen();
eq(g.next().value, 1, 'gen 1'); eq(g.next(5).value, 10, 'gen send'); var last = g.next(); eq(last.value + last.done, 'endtrue', 'gen return');
function* inner() { yield 'a'; yield 'b'; return 'r'; }
function* outerg() { var r = yield* inner(); yield r; }
deq([...outerg()], ['a', 'b', 'r'], 'yield*');
function* fib() { var [x, y] = [0, 1]; for (;;) { yield x; [x, y] = [y, x + y]; } }
var fs = []; for (var v of fib()) { if (v > 50) break; fs.push(v); }
eq(fs.join(), '0,1,1,2,3,5,8,13,21,34', 'infinite generator + break');
var gt = (function* () { try { yield 1; yield 2; } finally { fs = 'cleaned'; } })();
gt.next(); gt.return(7);
eq(fs, 'cleaned', 'generator return runs finally');
var gth = (function* () { try { yield 1; } catch (e) { yield 'caught ' + e; } })();
gth.next(); eq(gth.throw('x').value, 'caught x', 'generator throw');
// Map / Set / WeakMap
var m = new Map([[1, 'a'], ['1', 'b']]);
m.set(NaN, 'n');
eq(m.get(1), 'a', 'map get'); eq(m.get('1'), 'b', 'map string key'); eq(m.get(NaN), 'n', 'map NaN'); eq(m.size, 3, 'size');
m.delete(1); deq([...m.keys()].map(String), ['1', 'NaN'], 'map keys after delete');
var st = new Set([1, 2, 2, 3]); eq(st.size, 3, 'set dedup'); st.add(-0); eq(st.has(0), true, 'set -0');
var wm = new WeakMap(), kk = {}; wm.set(kk, 'v'); eq(wm.get(kk), 'v', 'weakmap');
deq(Object.entries({ a: 1, b: 2 }), [['a', 1], ['b', 2]], 'entries');
deq(Object.fromEntries([['x', 1]]), { x: 1 }, 'fromEntries');
deq(Array.from({ length: 3 }, (_, i) => i * i), [0, 1, 4], 'Array.from');
eq([1, 2, 3].includes(2), true, 'includes');
eq('abc'.padStart(5, '-'), '--abc', 'padStart');
eq(Object.assign({}, { a: 1 }, null, { b: 2 }).b, 2, 'assign');
var frozen = Object.freeze({ z: 1 }); frozen.z = 2; eq(frozen.z, 1, 'freeze');
eq(Number.isInteger(5.0), true, 'isInteger');
eq(new.target, undefined, 'new.target at top');
function NT() { return new.target === NT; } eq(new NT() instanceof NT, true, 'new.target ctor');
label: { if (true) break label; check(false, 'labeled block'); }
eq([1, 2, 3].at(-1), 3, 'at');
eq('x'.repeat(3), 'xxx', 'repeat');
eq(`${[1, 2]}`, '1,2', 'template array');
eq(String.raw`a\nb`, 'a\\nb', 'String.raw');
function tag(s, ...v) { return s.raw.join('|') + v.join(','); }
eq(tag`x${1}y${2}z`, 'x|y|z1,2', 'tagged template');
print('es6 done');
