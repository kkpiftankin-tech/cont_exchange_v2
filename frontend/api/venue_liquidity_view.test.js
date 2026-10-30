// Self-contained node-тест: `node venue_liquidity_view.test.js` (у frontend/api нет jest).
const { buildLiquidityStatsView, splitSymbol } = require('./venue_liquidity_view');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

// splitSymbol: длинный суффикс USDT раньше USD; кросс-пара по BTC; незнакомая котировка не ломается.
check('BTCUSDT → BTC/USDT', JSON.stringify(splitSymbol('BTCUSDT')) === JSON.stringify({ base: 'BTC', quote: 'USDT' }));
check('ETHBTC → ETH/BTC', JSON.stringify(splitSymbol('ETHBTC')) === JSON.stringify({ base: 'ETH', quote: 'BTC' }));
check('SOLUSD → SOL/USD', JSON.stringify(splitSymbol('SOLUSD')) === JSON.stringify({ base: 'SOL', quote: 'USD' }));
check('XYZ без известной котировки', JSON.stringify(splitSymbol('XYZ')) === JSON.stringify({ base: 'XYZ', quote: null }));
check('символ целиком = котировка не режется', splitSymbol('BTC').quote === null);

const now = Date.parse('2026-10-20T12:00:00Z');
const rows = [
  { venue: 'kraken', symbol: 'ETHUSDT', tape_quote_per_s: 12.5, tape_samples: '40', fill_coef: 0.2, fill_weight: 8.4, fill_orders: '9',
    updated_at: new Date(now - 5000) },
  { venue: 'binance', symbol: 'BTCUSDT', tape_quote_per_s: 21000, tape_samples: '300', fill_coef: 0.72, fill_weight: 3.1, fill_orders: '120',
    updated_at: new Date(now - 4000).toISOString() },
  // Лента есть, заявок ещё не было: f не показываем (нет данных, а не «1,0»).
  { venue: 'binance', symbol: 'ETHBTC', tape_quote_per_s: 0.5, tape_samples: 12, fill_coef: 1, fill_weight: 0, fill_orders: 0,
    updated_at: new Date(now - 3000) },
  // Лента ещё не читалась (0 выборок), запись давно не обновлялась.
  { venue: 'coinbase', symbol: 'BTCUSD', tape_quote_per_s: 0, tape_samples: 0, fill_coef: 1, fill_weight: 0, fill_orders: 0,
    updated_at: new Date(now - 600000) },
  null,
  { venue: '', symbol: 'X' },
];
const view = buildLiquidityStatsView(rows, now);

check('биржи отсортированы', view.map((v) => v.venue).join(',') === 'binance,coinbase,kraken');
const bin = view[0];
check('пары binance отсортированы', bin.pairs.map((p) => p.symbol).join(',') === 'BTCUSDT,ETHBTC');
check('bigint-строки приведены к числам', bin.pairs[0].tape_samples === 300 && bin.pairs[0].fill_orders === 120);
check('оборот в минуту = ×60', bin.pairs[0].tape_quote_per_min === 21000 * 60);
check('f и вес перенесены как есть', bin.pairs[0].fill_coef === 0.72 && bin.pairs[0].fill_weight === 3.1);
check('возраст записи в мс', bin.pairs[0].age_ms === 4000 && bin.pairs[0].stale === false);
check('нет заявок ⇒ f = null, has_fill = false', bin.pairs[1].has_fill === false && bin.pairs[1].fill_coef === null);
check('кросс-пара: котировка BTC', bin.pairs[1].quote === 'BTC' && bin.pairs[1].base === 'ETH');
const cb = view[1].pairs[0];
check('нет выборок ленты ⇒ has_tape = false, числа null', cb.has_tape === false && cb.tape_quote_per_s === null && cb.tape_quote_per_min === null);
check('давняя запись помечена stale', cb.stale === true && cb.age_ms === 600000);
check('пустой/некорректный ввод → []', buildLiquidityStatsView(null, now).length === 0 && buildLiquidityStatsView(undefined, now).length === 0);
check('нет updated_at ⇒ stale', buildLiquidityStatsView([{ venue: 'v', symbol: 'BTCUSDT', tape_samples: 1, tape_quote_per_s: 1 }], now)[0].pairs[0].stale === true);

if (failed) { console.error(failed + ' check(s) failed'); process.exit(1); }
console.log('all passed');
