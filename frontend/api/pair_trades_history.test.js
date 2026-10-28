// Self-contained node-тест: `node pair_trades_history.test.js` (у frontend/api нет jest).
const { bucketMsForHours, splitPair, buildPairTradesDto } = require('./pair_trades_history');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}
const near = (a, b, e = 1e-9) => Math.abs(a - b) <= e;

check('интервал по окну: 1 ч → 1 мин, 6 ч → 5 мин, 24 ч → 15 мин, 7 дн → 1 ч',
  bucketMsForHours(1) === 60000 && bucketMsForHours(6) === 300000 && bucketMsForHours(24) === 900000 && bucketMsForHours(168) === 3600000);
check('интервал: нечисло → по умолчанию 15 мин', bucketMsForHours('x') === 900000 && bucketMsForHours(0) === 900000);
check('пара делится на базу и вторую валюту', JSON.stringify(splitPair('ETH/BTC')) === JSON.stringify({ base: 'ETH', quote: 'BTC' }) && splitPair('XBTUSD').quote === '');

// Пример §3 карточки: две покупки в 10:00 (0,10 по 80 000 и 0,30 по 80 040) и продажа 0,25 по 80 100 в 10:01.
const T0 = 1793200000000;
const rows = [
  { symbol: 'BTC/USDT', t: T0 + 60000, buy_qty: null, sell_qty: 0.25, buy_notional: null, sell_notional: 0.25 * 80100, fills: 1 },
  { symbol: 'BTC/USDT', t: T0, buy_qty: 0.4, sell_qty: null, buy_notional: 0.1 * 80000 + 0.3 * 80040, sell_notional: null, fills: 2 },
  { symbol: 'ETH/BTC', t: T0, buy_qty: 2, sell_qty: 1, buy_notional: 2 * 0.051, sell_notional: 1 * 0.0515, fills: 4 },
  { symbol: 'SOL/USDT', t: T0, buy_qty: 0, sell_qty: 0, buy_notional: 0, sell_notional: 0, fills: 0 },   // пустой интервал не попадает
  { symbol: '', t: T0, buy_qty: 1 },
  null,
];
const dto = buildPairTradesDto(rows, 1, T0 + 3600000, 60000);
check('пары: пустая и без символа пропущены, сортировка по числу исполнений', dto.pairs.map((p) => p.pair).join() === 'ETH/BTC,BTC/USDT');
const btc = dto.pairs.find((p) => p.pair === 'BTC/USDT');
check('точки по возрастанию времени', btc.points.length === 2 && btc.points[0].t === T0 && btc.points[1].t === T0 + 60000);
check('цена покупки интервала — средневзвешенная: 80 030', near(btc.points[0].buy_price, 80030, 1e-6));
check('в интервале без покупок цены покупки нет (null, не ноль)', btc.points[1].buy_price === null && near(btc.points[1].sell_price, 80100, 1e-9));
check('в интервале без продаж цены продажи нет', btc.points[0].sell_price === null);
check('накопленные: в 10:00 общий 0,40 и нетто +0,40', near(btc.points[0].cum_total_qty, 0.4) && near(btc.points[0].cum_net_qty, 0.4));
check('накопленные: в 10:01 общий 0,65, нетто +0,15, покупки 0,40, продажи 0,25',
  near(btc.points[1].cum_total_qty, 0.65) && near(btc.points[1].cum_net_qty, 0.15) && near(btc.points[1].cum_buy_qty, 0.4) && near(btc.points[1].cum_sell_qty, 0.25));
check('итоги пары за окно', near(btc.totals.total_qty, 0.65) && near(btc.totals.net_qty, 0.15) && near(btc.totals.buy_price, 80030, 1e-6) && near(btc.totals.sell_price, 80100) && btc.fills === 3);
const eth = dto.pairs.find((p) => p.pair === 'ETH/BTC');
check('кросс-пара: цены в единицах второй валюты, база и котировка', eth.base === 'ETH' && eth.quote === 'BTC' && near(eth.points[0].buy_price, 0.051) && near(eth.points[0].sell_price, 0.0515));
check('накопленный общий объём не убывает', btc.points.every((p, i, a) => i === 0 || p.cum_total_qty >= a[i - 1].cum_total_qty));
check('окно, интервал, начало и первая точка', dto.window_hours === 1 && dto.bucket_ms === 60000 && dto.first_ms === T0 && dto.since_ms === T0 + 3600000 - 3600000 && dto.has_data === true);
check('нет строк → has_data=false и пустой список пар', (() => { const e = buildPairTradesDto([], 24, 5); return e.has_data === false && e.pairs.length === 0 && e.first_ms === null && e.bucket_ms === 900000; })());
check('не массив → безопасный пустой ответ', buildPairTradesDto(null, 24, 5).pairs.length === 0);
check('строковые числа SQL (float8 как строка) читаются', (() => {
  const d = buildPairTradesDto([{ symbol: 'BTC/USDT', t: String(T0), buy_qty: '1.5', sell_qty: '0', buy_notional: '120000', sell_notional: '0', fills: '2' }], 1, 5, 60000);
  return d.pairs[0].points[0].buy_price === 80000 && d.pairs[0].fills === 2;
})());

if (failed) { console.error(failed + ' check(s) failed'); process.exit(1); }
console.log('all passed');
