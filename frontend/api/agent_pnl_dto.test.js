// Self-contained node-тест: `node agent_pnl_dto.test.js` (у frontend/api нет jest).
const { buildAgentPnlDto, groupDto, decOrNull } = require('./agent_pnl_dto');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

// Decimal как его отдаёт proto-loader: units — строка (longs: String), scale — число.
const D = (units, scale) => ({ units: String(units), scale });
const grp = (r, u) => ({ has_data: true, r: D(r, 2), u: D(u, 2), r_plus_u: D(r + u, 2) });
const NO = { has_data: false, r: null, u: null, r_plus_u: null };

check('Decimal → число: 6000 с масштабом 2 = 60', decOrNull(D(6000, 2)) === 60);
check('Decimal отрицательный', decOrNull(D(-2000, 2)) === -20);
check('нет значения → null', decOrNull(null) === null && decOrNull(undefined) === null);
check('нечисловые units → null', decOrNull({ units: 'abc', scale: 2 }) === null);

check('группа без данных → null, не ноль', JSON.stringify(groupDto(NO)) === JSON.stringify(NO));
check('нулевое значение при has_data — это ноль, не null', groupDto({ has_data: true, r: D(0, 2), u: D(0, 2), r_plus_u: D(0, 2) }).r === 0);
check('has_data=true передаёт r, u, r_plus_u без пересчёта', (() => {
  const g = groupDto({ has_data: true, r: D(6000, 2), u: D(4000, 2), r_plus_u: D(777, 2) });
  return g.r === 60 && g.u === 40 && g.r_plus_u === 7.77;  // r_plus_u берётся ровно из ответа ledger, не r + u
})());

const resp = {
  currency: 'USDT', enabled: true,
  agents: [
    { agent_id: 'A_BTC_binancekraken', agent_kind: 'arbitrageur', asset: 'BTC', quote: 'USDT', venues: ['binance', 'kraken'],
      internal: grp(999, 0), external: grp(0, 0), total: grp(999, 0), mark_age_ms: '1200', ext_fills: 0, since_ms: '1792420000000' },
    { agent_id: 'T_ETH_okx', agent_kind: 'translator', asset: 'ETH', quote: 'USDT', venues: ['okx'],
      internal: NO, external: NO, total: NO, mark_age_ms: '-1', ext_fills: 0, since_ms: '0' },
    { agent_id: 'T_BTC_binance', agent_kind: 'translator', asset: 'BTC', quote: 'USDT', venues: ['binance'],
      internal: grp(6000, 4000), external: grp(4000, 0), total: grp(10000, 4000), mark_age_ms: '1500', ext_fills: 3, since_ms: '1792420000000' },
  ],
  totals: { internal: grp(6999, 4000), external: grp(4000, 0), total: grp(10999, 4000), agent_count: 3, incomplete_agents: 1 },
};
const dto = buildAgentPnlDto(resp, '2026-10-28T12:00:00.000Z');

check('порядок: переводчики по id, затем связки', dto.agents.map((a) => a.agent_id).join() === 'T_BTC_binance,T_ETH_okx,A_BTC_binancekraken');
check('метка времени ответа', dto.generatedAt === '2026-10-28T12:00:00.000Z');
check('валюта и выключатель', dto.currency === 'USDT' && dto.enabled === true);
const t = dto.agents[0];
check('внутренняя группа переводчика как в ответе ledger', t.internal.r === 60 && t.internal.u === 40 && t.internal.r_plus_u === 100);
check('итого агента — из ответа ledger', t.total.r === 100 && t.total.u === 40 && t.total.r_plus_u === 140);
check('int64-строки → числа', t.mark_age_ms === 1500 && t.since_ms === 1792420000000 && t.ext_fills === 3);
check('агент без цены: группы null, возраст -1 → null, начало 0 → null', (() => {
  const e = dto.agents[1];
  return !e.internal.has_data && e.internal.r === null && !e.total.has_data && e.mark_age_ms === null && e.since_ms === null;
})());
check('связка: две площадки', dto.agents[2].venues.length === 2);
check('строка «все агенты» — из ответа ledger без пересчёта', dto.totals.total.r_plus_u === 149.99 && dto.totals.internal.r === 69.99);
check('счётчики итогов', dto.totals.agent_count === 3 && dto.totals.incomplete_agents === 1);

const off = buildAgentPnlDto({ currency: 'USDT', enabled: false, agents: [], totals: {} }, 'x');
check('выключено: пустой список и enabled=false', off.enabled === false && off.agents.length === 0);
check('нет ответа → безопасный пустой DTO', buildAgentPnlDto(null, 'x').agents.length === 0);
check('неизвестный вид — в конец', buildAgentPnlDto({ agents: [{ agent_id: 'Z', agent_kind: '' }, { agent_id: 'A', agent_kind: 'translator' }] }, 'x')
  .agents.map((a) => a.agent_id).join() === 'A,Z');

if (failed > 0) { console.error(`\n${failed} check(s) failed`); process.exit(1); }
console.log('\nall agent_pnl_dto checks passed');
