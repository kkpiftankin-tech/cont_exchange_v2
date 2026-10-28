// Self-contained node-тест: `node pair_trades_history.test.js` (у frontend/api нет jest).
const { bucketMsForHours, splitPair, buildPairTradesDto, resolveFeeBps, DEFAULT_MAKER_BPS, DEFAULT_TAKER_BPS } = require('./pair_trades_history');

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

// --- комиссия на цену покупки (CALC-CE-PAIR-TRADES §2.1): расчётная, по типу заявки (maker — лимитная, taker — рыночная)
// Интервал: лимитная покупка 0,10 BTC по 80 000 (maker 2 б.п.) и рыночная 0,30 BTC по 80 040 (taker 10 б.п.).
const NOTIONAL_L = 0.1 * 80000, NOTIONAL_M = 0.3 * 80040;
const FEE = NOTIONAL_L * 0.0002 + NOTIONAL_M * 0.001;                  // 1,6 + 24,012 = 25,612 USDT
const feeDto = buildPairTradesDto([
  { symbol: 'BTC/USDT', t: T0, buy_qty: 0.4, sell_qty: 0.25, buy_notional: NOTIONAL_L + NOTIONAL_M, sell_notional: 0.25 * 80100, buy_fee: FEE, fills: 3 },
  { symbol: 'BTC/USDT', t: T0 + 60000, buy_qty: 0, sell_qty: 0.1, buy_notional: null, sell_notional: 0.1 * 80200, buy_fee: null, fills: 1 },
], 1, T0 + 3600000, 60000, { maker_bps: 2, taker_bps: 10, source: 'config' });
const fp = feeDto.pairs[0];
check('комиссия: цена покупки с комиссией = цена + комиссия на единицу базовой (80 030 + 25,612/0,4 = 80 094,03)', near(fp.points[0].buy_price_fee, 80030 + FEE / 0.4, 1e-6) && near(fp.points[0].buy_price, 80030, 1e-6));
check('комиссия: надбавка положительна и меньше цены покупки в разы (≈ 8 б.п. от цены)', fp.points[0].buy_price_fee > fp.points[0].buy_price && (fp.points[0].buy_price_fee / fp.points[0].buy_price - 1) < 0.002);
check('комиссия: интервал без покупок — цены с комиссией нет (null, не ноль); цена продажи комиссией не меняется', fp.points[1].buy_price_fee === null && near(fp.points[1].sell_price, 80200));
check('комиссия: итоги пары — цена покупки с комиссией и сумма комиссии', near(fp.totals.buy_fee, FEE, 1e-9) && near(fp.totals.buy_price_fee, 80030 + FEE / 0.4, 1e-6));
check('комиссия: ставки и источник возвращаются в ответе', feeDto.fee.maker_bps === 2 && feeDto.fee.taker_bps === 10 && feeDto.fee.source === 'config');
check('комиссия: ставок нет — комиссия неизвестна: цена с комиссией и сумма комиссии null (не равны цене без комиссии, не ноль)', (() => {
  const d = buildPairTradesDto([{ symbol: 'BTC/USDT', t: T0, buy_qty: 1, buy_notional: 80000, fills: 1 }], 1, 5, 60000);
  return d.fee.maker_bps === null && d.fee.source === 'default' && d.pairs[0].points[0].buy_price_fee === null && d.pairs[0].totals.buy_fee === null
    && d.pairs[0].points[0].buy_price === 80000;
})());

// --- разбор ставок: конфигурация > env > по умолчанию; «не больше нуля» — не задано (как в ledger)
check('ставки по умолчанию: 2 и 10 б.п., источник default', (() => { const f = resolveFeeBps(null, {}); return f.maker_bps === DEFAULT_MAKER_BPS && f.taker_bps === DEFAULT_TAKER_BPS && f.source === 'default' && DEFAULT_MAKER_BPS === 2 && DEFAULT_TAKER_BPS === 10; })());
check('ставки: обе из конфигурации — источник config', (() => { const f = resolveFeeBps({ ce_maker_fee_bps: '3', ce_taker_fee_bps: '12.5' }, {}); return f.maker_bps === 3 && f.taker_bps === 12.5 && f.source === 'config'; })());
check('ставки: taker не задан (−1 или 0), maker из конфигурации — источник mixed, taker по умолчанию', (() => {
  const a = resolveFeeBps({ ce_maker_fee_bps: 2, ce_taker_fee_bps: -1 }, {});
  const b = resolveFeeBps({ ce_maker_fee_bps: 2, ce_taker_fee_bps: 0 }, {});
  return a.maker_bps === 2 && a.taker_bps === 10 && a.source === 'mixed' && b.source === 'mixed';
})());
check('ставки: env подставляется вместо значений по умолчанию, но не вместо конфигурации', (() => {
  const f = resolveFeeBps({ ce_maker_fee_bps: 4, ce_taker_fee_bps: -1 }, { CE_BAND_MAKER_FEE_BPS: '9', CE_BAND_REF_FEE_BPS: '15' });
  return f.maker_bps === 4 && f.taker_bps === 15 && f.source === 'mixed';
})());
check('ставки: нечисловой, пустой и нулевой env игнорируются', (() => { const f = resolveFeeBps(null, { CE_BAND_MAKER_FEE_BPS: '', CE_BAND_REF_FEE_BPS: 'abc' }); return f.maker_bps === 2 && f.taker_bps === 10 && f.source === 'default'; })());
check('комиссия: кросс-пара — надбавка в единицах второй валюты (BTC)', (() => {
  const d = buildPairTradesDto([{ symbol: 'ETH/BTC', t: T0, buy_qty: 2, buy_notional: 2 * 0.051, buy_fee: 2 * 0.051 * 0.0002, fills: 1 }], 1, 5, 60000, { maker_bps: 2, taker_bps: 10, source: 'default' });
  return near(d.pairs[0].points[0].buy_price_fee, 0.051 * 1.0002, 1e-12);
})());

if (failed) { console.error(failed + ' check(s) failed'); process.exit(1); }
console.log('all passed');
