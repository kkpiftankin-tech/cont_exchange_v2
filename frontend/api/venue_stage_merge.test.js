// Self-contained node-тест: `node venue_stage_merge.test.js` (у frontend/api нет jest).
const { mergeVenueStages } = require('./venue_stage_merge');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

const venues = [
  { venue_id: 'binance', avg_period_ms: 24000, pairs: [{ symbol: 'BTC/USDT', period_ms: 24000 }, { symbol: 'ETH/USDT', period_ms: 24000 }] },
  { venue_id: 'okx', avg_period_ms: 23000, pairs: [{ symbol: 'BTC/USDT', period_ms: 23000 }] },
];
const admin = [{
  config: { venue_id: 'binance' },
  metrics: {
    cycle_ms: 19000.5, cycle_ms_ema: 18000, cycle_end_ms: 1700000019000,
    stages: {
      'BTC/USDT': { lock_wait_ms: 1, fetch_ms: 2100, rest_depth_ms: 1000, rest_ticker_ms: 1000, rest_trades_ms: 0,
        raw_publish_ms: 3, snapshot_ms: 40, curve_ms: 90, total_ms: 2250, rest_requests: 2, ticker_state: 0, recv_ms: 1700000010000 },
    },
  },
}];
const r = mergeVenueStages(venues, admin, 1700000013000);

check('binance: cycle_ms перенесён', r[0].cycle_ms === 19000.5 && r[0].stages_available === true);
check('binance BTC: стадии перенесены без пересчёта', r[0].pairs[0].stages.fetch_ms === 2100 && r[0].pairs[0].stages.curve_ms === 90 && r[0].pairs[0].stages.total_ms === 2250);
check('binance BTC: возраст книги = now − recv', r[0].pairs[0].stages.book_age_ms === 3000);
check('binance ETH: нет стадий ⇒ null (не выдумываем)', r[0].pairs[1].stages === null);
check('okx: нет метрик venues ⇒ stages_available=false, cycle null', r[1].stages_available === false && r[1].cycle_ms === null && r[1].pairs[0].symbol === 'BTC/USDT');
check('исходные поля сохранены', r[0].avg_period_ms === 24000 && r[0].pairs[0].period_ms === 24000);
check('мусор не ломает слияние', mergeVenueStages(venues, null, 1).length === 2 && mergeVenueStages(null, admin, 1).length === 0);
check('нечисловые значения ⇒ null', mergeVenueStages(venues, [{ config: { venue_id: 'okx' }, metrics: { cycle_ms: 'x', stages: { 'BTC/USDT': { fetch_ms: 'abc', recv_ms: 0 } } } }], 5)[1].pairs[0].stages.fetch_ms === null);

const fresh = mergeVenueStages(venues, [{ config: { venue_id: 'binance' }, metrics: { cycle_ms: 0, cycle_ms_ema: 0, cycle_end_ms: 0, stages: {} } }], 5);
check('до первого завершённого прохода cycle_ms = null, а не 0', fresh[0].cycle_ms === null && fresh[0].cycle_end_ms === null && fresh[0].stages_available === true);
const noRest = mergeVenueStages(venues, [{ config: { venue_id: 'binance' }, metrics: { cycle_ms: 100, cycle_end_ms: 9, stages: { 'BTC/USDT': { fetch_ms: 5000, total_ms: 5001 } } } }], 5);
check('нет разбивки REST ⇒ rest_* = null (не 0), fetch_ms сохранён', noRest[0].pairs[0].stages.rest_depth_ms === null && noRest[0].pairs[0].stages.fetch_ms === 5000);

if (failed) { console.error(failed + ' FAILED'); process.exit(1); }
console.log('venue_stage_merge.test: OK');
