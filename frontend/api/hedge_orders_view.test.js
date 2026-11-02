// Self-contained node-тест: `node hedge_orders_view.test.js` (у frontend/api нет jest).
const { buildHedgeMap, mapChildStatus } = require('./hedge_orders_view');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

check('PENDING → NEW', mapChildStatus('PENDING') === 'NEW');
check('CANCELLED → EXPIRED', mapChildStatus('CANCELLED') === 'EXPIRED');
check('FILLED остаётся', mapChildStatus('FILLED') === 'FILLED' && mapChildStatus('PARTIALLY_FILLED') === 'PARTIALLY_FILLED');
check('пустой статус → null', mapChildStatus(null) === null && mapChildStatus('') === null);

const rows = [
  { agent_id: 'T_BTC_binance', venue: 'binance', symbol: 'BTCUSDT', side: 'SELL', target_qty: '0.5', filled_qty: '0.3', status: 'PARTIALLY_FILLED', created_ms: 1000 },
  { agent_id: 'A_BTC_binancekraken', venue: 'binance', symbol: 'BTCUSDT', side: 'SELL', target_qty: '1', filled_qty: '1', status: 'FILLED', created_ms: 2000 },
  { agent_id: 'A_BTC_binancekraken', venue: 'kraken', symbol: 'BTCUSDT', side: 'BUY', target_qty: '1', filled_qty: '0', status: 'CANCELLED', created_ms: 3000 },
  null,
  { venue: 'okx' },
];
const m = buildHedgeMap(rows);
check('агент без id и пустые строки пропущены', m.size === 2);
const t = m.get('T_BTC_binance');
check('переводчик: числа и статус', t.hedge_sent === 0.5 && t.hedge_filled === 0.3 && t.hedge_status === 'PARTIALLY_FILLED' && t.hedge_venue === 'binance');
const a = m.get('A_BTC_binancekraken');
check('арбитражёр: верхний уровень = самая свежая заявка (kraken)', a.hedge_venue === 'kraken' && a.hedge_status === 'EXPIRED' && a.hedge_filled === 0);
check('арбитражёр: есть заявка на каждой бирже', a.hedge_by_venue.binance.filled === 1 && a.hedge_by_venue.kraken.sent === 1 && a.hedge_by_venue.binance.status === 'FILLED');
check('нечисловые значения → 0', buildHedgeMap([{ agent_id: 'x', venue: 'v', target_qty: 'abc', filled_qty: null, status: 'FILLED', created_ms: 1 }]).get('x').hedge_sent === 0);
check('не массив → пустая карта', buildHedgeMap(null).size === 0 && buildHedgeMap(undefined).size === 0);

if (failed) { console.error(failed + ' check(s) failed'); process.exit(1); }
console.log('all passed');
