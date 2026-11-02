// Self-contained node-тест: `node slope_aggregate.test.js` (у frontend/api нет jest).
const assert = require('assert');
const { aggregateLines } = require('./slope_aggregate');

let failed = 0;
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}
const q = (l, p) => l.k * p + l.c;  // объём линии при цене p

// Числовой пример (карточка CALC-F05A-SLOPE-ONDEMAND): v1: anchor 100, β=0.5 ⇒ q=2p−200;
// v2: anchor 102, β=1 ⇒ q=p−102. При p=101: v1=+2, v2=−1, сумма=+1 (= 3p − 302).
const r = aggregateLines([
  { venue: 'v1', anchor: 100, betaT: 0.5 },
  { venue: 'v2', anchor: 102, betaT: 1 },
], 'v1');
check('две линии', r.lines.length === 2);
check('v1: q(101)=+2', near(q(r.lines[0], 101), 2));
check('v2: q(101)=−1', near(q(r.lines[1], 101), -1));
check('Σ все: k=3, c=−302', near(r.all.k, 3) && near(r.all.c, -302) && r.all.count === 2);
check('Σ все: q(101)=+1 = сумма линий', near(q(r.all, 101), q(r.lines[0], 101) + q(r.lines[1], 101)));
check('Σ все кроме v1 = линия v2', near(r.ex.k, 1) && near(r.ex.c, -102) && r.ex.excluded === 'v1' && r.ex.count === 1);
check('Σ все = (Σ без v1) + v1 при любой цене', [95, 100, 101, 110].every((p) => near(q(r.all, p), q(r.ex, p) + q(r.lines[0], p))));
check('нулевой объём при цене якоря своей биржи', near(q(r.lines[0], 100), 0) && near(q(r.lines[1], 102), 0));

// Выбранной биржи нет среди данных ⇒ «все кроме» = «все», excluded=null.
const r2 = aggregateLines([{ venue: 'v1', anchor: 100, betaT: 0.5 }], 'nope');
check('исключаемой нет ⇒ ex = all', near(r2.ex.k, r2.all.k) && near(r2.ex.c, r2.all.c) && r2.ex.excluded === null);

// Некорректные биржи пропускаются, остальные считаются.
const r3 = aggregateLines([
  { venue: 'a', anchor: 100, betaT: 0 }, { venue: 'b', anchor: NaN, betaT: 1 },
  { venue: 'c', anchor: 100, betaT: -2 }, { venue: 'd', anchor: 100, betaT: 2 }, null, { venue: 'e' },
], 'd');
check('мусор пропущен', r3.lines.length === 1 && r3.lines[0].venue === 'd');
check('все кроме единственной = пустая сумма', r3.ex.count === 0 && r3.ex.k === 0 && r3.ex.c === 0);
check('пустой/не массив → нули', aggregateLines([], 'x').all.count === 0 && aggregateLines(null, 'x').all.k === 0);

assert.strictEqual(failed, 0, failed + ' failed');
console.log('slope_aggregate tests passed');
