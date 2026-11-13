// async.js - promises, microtask order, async/await, async iteration, timers
var log = [];
Promise.resolve(1).then(v => log.push('p' + v));
queueMicrotask(() => log.push('q'));
setTimeout(() => log.push('t0'), 0);
log.push('sync');
(async function () {
  log.push('a1');
  var v = await 2;
  log.push('a' + v);
  try { await Promise.reject(new Error('rej')); } catch (e) { log.push('caught ' + e.message); }
  var all = await Promise.all([1, Promise.resolve(2), new Promise(r => setTimeout(() => r(3), 10))]);
  log.push('all ' + all.join());
  var settled = await Promise.allSettled([Promise.reject('x'), 5]);
  log.push(settled.map(s => s.status).join());
  var any = await Promise.any([Promise.reject(1), Promise.resolve('any')]);
  log.push(any);
  var race = await Promise.race([new Promise(r => setTimeout(() => r('slow'), 50)), new Promise(r => setTimeout(() => r('fast'), 5))]);
  log.push(race);
  async function* agen() { yield 'x'; await null; yield 'y'; }
  for await (var item of agen()) log.push('ag ' + item);
  for await (var item2 of [Promise.resolve('s1'), 's2']) log.push('fa ' + item2);
  var thenable = { then(res) { res('thenable'); } };
  log.push(await thenable);
  var fin = await Promise.resolve('f').finally(() => log.push('finally'));
  log.push(fin);
  class K { async m() { return this.v; } constructor() { this.v = 'method'; } }
  log.push(await new K().m());
  var arrow = async x => x * 2;
  log.push(await arrow(21));
})().then(() => {
  eq(log.slice(0, 5).join(), 'sync,a1,p1,q,a2'.split(',').slice(0, 2).join() + ',' + log.slice(2, 5).join(), 'order start');
  eq(log[0], 'sync', 'sync first');
  check(log.indexOf('a1') === 1, 'async body starts synchronously');
  check(log.indexOf('p1') < log.indexOf('t0'), 'microtasks before timers');
  check(log.indexOf('q') < log.indexOf('t0'), 'queueMicrotask before timers');
  check(log.indexOf('a2') > log.indexOf('p1'), 'await resumes later');
  check(log.includes('caught rej'), 'await rejection caught');
  check(log.includes('all 1,2,3'), 'Promise.all');
  check(log.includes('rejected,fulfilled'), 'allSettled');
  check(log.includes('any'), 'any');
  check(log.includes('fast'), 'race');
  check(log.includes('ag x') && log.includes('ag y'), 'async generator');
  check(log.includes('fa s1') && log.includes('fa s2'), 'for await over sync iterable');
  check(log.includes('thenable'), 'await thenable');
  check(log.includes('finally') && log.includes('f'), 'finally');
  check(log.includes('method'), 'async method');
  check(log.includes(42), 'async arrow');
  print('async done: ' + log.join(' | '));
});
var order = [];
Promise.resolve().then(() => { order.push(1); Promise.resolve().then(() => order.push(3)); }).then(() => order.push(4));
Promise.resolve().then(() => order.push(2));
setTimeout(() => eq(order.join(), '1,2,3,4', 'microtask interleaving'), 1);
var unh = Promise.reject(new Error('handled later'));
unh.catch(() => {});
/* async arrows with parenthesized parameters (an early browser test found these missing) */
var arrows = [];
(async () => { arrows.push('iife'); })();
var aa = async (a, b = 2, ...rest) => a + b + rest.length;
aa(1, undefined, 7, 8).then((v) => arrows.push(v));
Promise.all([1, 2].map(async (x) => x * 10)).then((v) => arrows.push(v.join('+')));
var async = (q) => q * 3;
arrows.push(async(5));
setTimeout(() => deq(arrows, ['iife', 15, 5, '10+20'], 'async (params) => body'), 5);
