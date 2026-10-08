// Self-contained node-тест: `node book_vwap.test.js` (у frontend/api нет jest).
const assert = require('assert');
const { cumulativeBookVwap } = require('./book_vwap');

let failed = 0;
const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

// Числовой пример из CALC-BOOK-VWAP §3: ask-сторона, лучший уровень первым.
const asks = [{ p: 100, q: 2 }, { p: 101, q: 3 }, { p: 103, q: 5 }];
const r = cumulativeBookVwap(asks);
check('3 точки', r.length === 3);
check('cum = 2, 5, 10', r[0].cum === 2 && r[1].cum === 5 && r[2].cum === 10);
check('vwap#1 = 100', near(r[0].vwap, 100));
check('vwap#2 = (200+303)/5 = 100.6', near(r[1].vwap, 100.6));
check('vwap#3 = (200+303+515)/10 = 101.8', near(r[2].vwap, 101.8));
check('vwap монотонен от лучшей цены (ask растёт)', r[0].vwap <= r[1].vwap && r[1].vwap <= r[2].vwap);
check('vwap ∈ [лучшая цена, цена уровня]', r.every((x) => x.vwap >= asks[0].p - 1e-12 && x.vwap <= x.p + 1e-12));

// bid-сторона (цены убывают): vwap убывает, но не ниже цены уровня.
const bids = [{ p: 99, q: 1 }, { p: 98, q: 1 }];
const b = cumulativeBookVwap(bids);
check('bid vwap#2 = 98.5', near(b[1].vwap, 98.5));
check('bid vwap ≥ цены худшего уровня', b[1].vwap >= 98 - 1e-12);

// qtyKey: имя поля объёма (fetchVenueCurveImpl использует "v").
const v = cumulativeBookVwap([{ p: 10, v: 4 }, { p: 12, v: 4 }], 'v');
check('qtyKey "v"', near(v[1].vwap, 11) && v[1].cum === 8);

// Мусорные уровни пропускаются, не ломая накопление.
const dirty = cumulativeBookVwap([{ p: 100, q: 1 }, { p: NaN, q: 1 }, { p: 101, q: 0 }, { p: 102, q: 1 }]);
check('мусор пропущен', dirty.length === 2 && near(dirty[1].vwap, 101));
check('пустой/не массив → []', cumulativeBookVwap([]).length === 0 && cumulativeBookVwap(null).length === 0);

// Эквивалентность прежнему inline-коду buildSide (побитово) на случайных данных.
function legacy(levels) {
  let cumQty = 0, cumNotional = 0; const pts = [];
  for (let i = 0; i < levels.length; i++) {
    cumQty += levels[i].v; cumNotional += levels[i].p * levels[i].v;
    if (cumQty <= 0) continue;
    pts.push({ cumQty, vwap: cumNotional / cumQty });
  }
  return pts;
}
let same = true;
for (let t = 0; t < 200 && same; t++) {
  const lv = Array.from({ length: 1 + (t % 40) }, (_, i) => ({ p: 50000 + i * 0.01 + Math.random(), v: Math.random() * 5 + 1e-5 }));
  const a = legacy(lv), n = cumulativeBookVwap(lv, 'v');
  same = a.length === n.length && a.every((x, i) => x.cumQty === n[i].cum && x.vwap === n[i].vwap);
}
check('побитово совпадает с прежним buildSide', same);

assert.strictEqual(failed, 0, failed + ' failed');
console.log('book_vwap tests passed');
