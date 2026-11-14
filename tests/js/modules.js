// ES modules: import / export, live bindings, cycles, export *, default exports, import.meta,
// top-level await and dynamic import(). A small loader like the browser's (browser_jsrt.js).
var sources = {
  'main.js': `
    import def, { inc, count, Point as P } from './counter.js';
    import * as util from './util.js';
    import { a, b } from './cycle-a.js';
    import { stars } from './star.js';
    import './side.js';
    globalThis.log.push('main ' + def + ' ' + count);
    inc(); inc();
    globalThis.log.push('after inc ' + count + ' ' + new P(1, 2).sum());
    globalThis.log.push('util ' + util.twice(21) + ' ' + Object.keys(util).sort().join(','));
    globalThis.log.push('cycle ' + a() + ' ' + b());
    globalThis.log.push('star ' + stars + ' meta ' + import.meta.url);
    const late = await import('./late.js');
    globalThis.log.push('dynamic ' + late.default + ' ' + late.answer);
    export const done = true;
  `,
  'counter.js': `
    export let count = 0;
    export function inc() { count++; }
    export class Point { constructor(x, y) { this.x = x; this.y = y; } sum() { return this.x + this.y; } }
    export default 'counter';
  `,
  'util.js': `
    const twice = (x) => x * 2;
    export { twice, twice as double };
    export default function () { return 'anon'; }
  `,
  'cycle-a.js': `
    import { b } from './cycle-b.js';
    export function a() { return 'a+' + b.name; }
    export { b };
  `,
  'cycle-b.js': `
    import { a } from './cycle-a.js';
    export function b() { return 'b+' + a.name; }
  `,
  'star.js': `
    export * from './star2.js';
    export * as all from './star2.js';
  `,
  'star2.js': `
    export const stars = 5;
  `,
  'side.js': `
    globalThis.log.push('side effect');
    await null;
    globalThis.log.push('side after await');
  `,
  'shared-user.js': `
    import { slow } from './shared.js';
    globalThis.log2.push('user sees ' + slow);
  `,
  'shared.js': `
    import './shared-dep.js';
    export var slow = 'ready';
  `,
  'shared-dep.js': `
    await new Promise((r) => setTimeout(r, 2));
  `,
  'late.js': `
    export default 'late';
    export const answer = 42;
  `,
};
/* a big module: compiled a slice at a time while the script waits, the same as at once */
var big = 'export let total = 0;\n';
for (var i = 0; i < 600; i++)
  big += 'export function f' + i + '(a) { return a + ' + i + '; } const g' + i + ' = function () { return ' + i + '; };' +
         ' total += f' + i + '(1) + g' + i + '(); export const n' + i + ' = g' + i + '.name;\n';
sources['big.js'] = big;
globalThis.log = [];
var modules = new Map();
function resolve(spec, base) { return spec.replace(/^\.\//, ''); }
function load(url) {
  if (modules.has(url)) return modules.get(url);
  var rec = { url: url, ns: Object.create(null), deps: [], state: 'new' };
  modules.set(url, rec);
  var c = compileModule(sources[url], url);
  rec.fn = c[0];
  rec.deps = c[1].map(function (s) { return load(resolve(s, url)); });
  return rec;
}
function exportName(rec, name, g) {
  if (name === '*') {
    for (var k of Object.keys(g)) if (k !== 'default' && !(k in rec.ns)) Object.defineProperty(rec.ns, k, { get: (function (k) { return function () { return g[k]; }; })(k), enumerable: true });
    return;
  }
  Object.defineProperty(rec.ns, name, { get: g, enumerable: true });
}
function instantiate(rec) {
  if (rec.state !== 'new') return;
  rec.state = 'linking';
  rec.deps.forEach(instantiate);
  var go;
  var p = new Promise(function (r) { go = r; });
  rec.go = go;
  rec.done = rec.fn(function (i) { return rec.deps[i].ns; }, function (n, g) { exportName(rec, n, g); }, { url: 'test:' + rec.url, go: p });
  rec.state = 'linked';
}
function reaches(a, b, seen) {
  seen = seen || new Set();
  if (a === b) return true;
  if (seen.has(a)) return false;
  seen.add(a);
  return a.deps.some(function (d) { return reaches(d, b, seen); });
}
function evaluate(rec, chain) {
  if (rec.ev) return rec.ev;
  chain = chain ? chain.concat([rec]) : [rec];
  rec.state = 'evaluating';
  rec.ev = (async function () {
    for (var d of rec.deps) {
      if (chain.includes(d) || (d.state === 'evaluating' && reaches(d, rec))) continue;
      await evaluate(d, chain);
    }
    rec.go();
    await rec.done;
    rec.state = 'evaluated';
  })();
  return rec.ev;
}
async function run(url) { var rec = load(url); instantiate(rec); await evaluate(rec); return rec.ns; }
setImportHook(function (spec, from) { return run(resolve(spec, from)); });
/* two graphs share a module that is still running when the second one starts */
globalThis.log2 = [];
run('shared.js');
run('shared-user.js');
var result, bigNs;
run('main.js').then(function (ns) { result = ns; }, function (e) { result = e; });
run('big.js').then(function (ns) { bigNs = ns; }, function (e) { bigNs = e; });
setTimeout(function () {
  check(result && result.done === true, 'main module finished: ' + (result instanceof Error ? result : typeof result));
  deq(log, ['side effect', 'side after await', 'main counter 0', 'after inc 2 3', 'util 42 default,double,twice',
            'cycle a+b b+a', 'star 5 meta test:main.js', 'dynamic late 42'], 'module order and bindings');
  deq(log2, ['user sees ready'], 'a shared module still running is waited for');
  throws(function () { compileModule('export default 1; export default 2;', 'x'); }, SyntaxError, 'duplicate default export');
  compileModule('import { x } from "a"; x = 1;', 'y')[0](function () { return { x: 1 }; }, function () {}, { go: null })
    .then(function () { check(false, 'assignment to an import did not throw'); },
          function (e) { check(e instanceof TypeError, 'assignment to an import: ' + e); });
  throws(function () { eval('import x from "y"'); }, SyntaxError, 'import outside a module');
  check(big.length > 40000, 'the big module is big');
  eq(bigNs && bigNs.total, 600 * 600, 'big module ran' + (bigNs instanceof Error ? ' (' + bigNs + ')' : ''));
  eq(bigNs && bigNs.n599, 'g599', 'big module: names of functions compiled ahead');
  throws(function () { compileModule(big + 'export let 5;', 'bad.js'); }, SyntaxError, 'a big module with a syntax error');
  print('modules done');
}, 10);
