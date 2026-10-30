// Self-contained node-тест: `node agent_pnl_history.test.js` (у frontend/api нет jest).
const { pnlSamples, downsample, buildPnlHistoryDto, totalsSamples, buildTotalsHistoryDto, groupsOf } = require('./agent_pnl_history');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}

// --- pnlSamples: что попадает в историю
const rows = [
  { agent_id: 'T_BTC_binance', realized_usdt: 12.5, unrealized_usdt: -3.2, trade_fills: 40 },
  { agent_id: 'T_ETH_okx', realized_usdt: null, unrealized_usdt: null, pnl_usdt: null },          // нет данных → пропуск
  { agent_id: 'T_SOL_kraken', pnl_usdt: 7, unrealized_usdt: null, trade_fills: 3 },               // только R+U без R и U → не подменяет R, в выборку не идёт
  { agent_id: 'A_BTC_binancekraken', realized_usdt: 0, unrealized_usdt: 0, trade_fills: 0 },      // нули — это данные
  { agent_id: '', realized_usdt: 1 },                                                              // без id → пропуск
  null,
];
const s = pnlSamples(rows, 1700000000000);
check('в выборку идут агенты с данными: 2 из 6', s.length === 2);
check('R и U перенесены как есть', s[0].realized === 12.5 && s[0].unrealized === -3.2 && s[0].fills === 40);
check('нет R и U, есть только pnl_usdt → строки нет (R не подменяется суммой)', !s.some((x) => x.agent_id === 'T_SOL_kraken'));
check('нули записываются (0 ≠ нет данных)', s[1].agent_id === 'A_BTC_binancekraken' && s[1].realized === 0 && s[1].unrealized === 0);
check('метка времени выборки общая', s.every((x) => x.ts_ms === 1700000000000));
check('не массив → пусто', pnlSamples(null, 1).length === 0);

// --- downsample
const pts = Array.from({ length: 1000 }, (_, i) => ({ t: i * 1000, r: i, u: 0, total: i }));
const d = downsample(pts, 100);
check('прореживание до ≤ maxPoints', d.length <= 100 && d.length >= 90);
check('первая и последняя точки сохранены', d[0].t === 0 && d[d.length - 1].t === 999000);
check('порядок по времени', d.every((p, i) => i === 0 || p.t > d[i - 1].t));
check('короткий ряд не меняется', downsample(pts.slice(0, 5), 100).length === 5);
check('одна точка во времени', downsample([{ t: 5, r: 1, u: 1, total: 2 }, { t: 5, r: 2, u: 2, total: 4 }], 1).length >= 1);
check('несортированный вход сортируется', downsample([{ t: 3 }, { t: 1 }, { t: 2 }], 10).map((p) => p.t).join() === '1,2,3');

// --- DTO
const dto = buildPnlHistoryDto('T_BTC_binance', [
  { ts_ms: 2000, realized_usdt: 2, unrealized_usdt: 1 },
  { ts_ms: 1000, realized_usdt: 1, unrealized_usdt: -1 },
  { ts_ms: 3000, realized_usdt: 3, unrealized_usdt: null },
  { ts_ms: 4000, realized_usdt: null, unrealized_usdt: null },   // пустая запись пропускается
  { ts_ms: 'x', realized_usdt: 1, unrealized_usdt: 1 },          // битое время пропускается
], 24, 10000000);
check('DTO: 3 валидные точки по возрастанию времени', dto.points.length === 3 && dto.points[0].t === 1000 && dto.points[2].t === 3000);
check('DTO: R+U = сумма частей', dto.points[0].total === 0 && dto.points[1].total === 3);
check('DTO: нет U ⇒ u = null и total = null (R+U не подменяется одним R), R остаётся', dto.points[2].u === null && dto.points[2].total === null && dto.points[2].r === 3);
check('DTO: окно и начало', dto.window_hours === 24 && dto.since_ms === 10000000 - 24 * 3600 * 1000 && dto.first_ms === 1000);
check('DTO: has_data', dto.has_data === true);
const empty = buildPnlHistoryDto('x', [], 24, 5);
check('DTO: пустая история → has_data=false, без точек', empty.has_data === false && empty.points.length === 0 && empty.first_ms === null);

// --- выгода в клиринге (ADR-083)
const sur = { agents: new Map([['A_BTC_binanceokx', { surplus: 1.5, tacts: 55 }], ['T_BTC_binance', { surplus: 0.25, tacts: 60 }], ['', { surplus: 9, tacts: 1 }]]), tacts: 60, expected: 62 };
const s2 = pnlSamples([
  { agent_id: 'T_BTC_binance', realized_usdt: 2, unrealized_usdt: 1, trade_fills: 7 },
  { agent_id: 'T_ETH_okx', realized_usdt: null, unrealized_usdt: null, pnl_usdt: null },
], 1700000000000, sur);
const byId = Object.fromEntries(s2.map((x) => [x.agent_id, x]));
check('выгода: агент с R/U получает и выгоду', byId.T_BTC_binance.realized === 2 && byId.T_BTC_binance.surplus_inc === 0.25 && byId.T_BTC_binance.surplus_tacts === 60 && byId.T_BTC_binance.surplus_expected === 62);
check('выгода: связка без внешних сделок попадает в выборку с R/U = null', byId.A_BTC_binanceokx && byId.A_BTC_binanceokx.realized === null && byId.A_BTC_binanceokx.unrealized === null && byId.A_BTC_binanceokx.surplus_inc === 1.5);
check('выгода: агент без данных и без выгоды пропущен, пустой id пропущен', !byId.T_ETH_okx && !('' in byId) && s2.length === 2);
check('выгода: без накопителя поведение прежнее (3 аргумента необязательны)', pnlSamples(rows, 1700000000000).length === 2 && pnlSamples(rows, 1)[0].surplus_inc === null);

const dto2 = buildPnlHistoryDto('A_BTC_binanceokx', [
  { ts_ms: 2000, realized_usdt: null, unrealized_usdt: null, surplus_inc_usdt: 2, surplus_tacts: 50, surplus_tacts_expected: 60 },
  { ts_ms: 1000, realized_usdt: null, unrealized_usdt: null, surplus_inc_usdt: 1, surplus_tacts: 55, surplus_tacts_expected: 60 },
  { ts_ms: 3000, realized_usdt: 4, unrealized_usdt: -1, surplus_inc_usdt: 3, surplus_tacts: 60, surplus_tacts_expected: 60 },
  { ts_ms: 4000, realized_usdt: null, unrealized_usdt: null, surplus_inc_usdt: null },
], 24, 10000000);
check('DTO выгода: накопленный ряд s = 1, 3, 6', dto2.points.map((p) => p.s).join() === '1,3,6');
check('DTO выгода: у связки без внешних сделок total = null, а не 0', dto2.points[0].total === null && dto2.points[0].r === null);
check('DTO выгода: когда R и U есть, total = R+U = 3', dto2.points[2].total === 3);
check('DTO выгода: итог и доля учтённых тактов (165 из 180)', dto2.surplus_total === 6 && Math.abs(dto2.surplus_coverage - 165 / 180) < 1e-12 && dto2.has_surplus === true);
check('DTO без выгоды: has_surplus=false, s = null', buildPnlHistoryDto('x', [{ ts_ms: 1, realized_usdt: 1, unrealized_usdt: 1 }], 24, 5).points[0].s === null && buildPnlHistoryDto('x', [{ ts_ms: 1, realized_usdt: 1, unrealized_usdt: 1 }], 24, 5).has_surplus === false);

// --- внутренняя группа агента (ADR-087, модель C′): пишется как есть, неизвестное — null
const sInt = pnlSamples([
  { agent_id: 'T_BTC_binance', realized_usdt: 93.3, unrealized_usdt: 36.7, pnl_internal: { has_data: true, u_has_data: true, r: 10, u: 0, r_plus_u: 10 } },
  { agent_id: 'T_ETH_okx', realized_usdt: null, unrealized_usdt: null, pnl_internal: { has_data: true, u_has_data: true, r: -2.5, u: 0 } },   // только внутренняя группа
  { agent_id: 'T_SOL_okx', realized_usdt: 1, unrealized_usdt: null, pnl_internal: { has_data: false, u_has_data: false, r: null, u: null } },
], 1700000000000);
check('внутренняя группа: R и U записаны из ledger (U = 0 — данные, а не подстановка)', sInt[0].internal_realized === 10 && sInt[0].internal_unrealized === 0);
check('агент только с внутренней группой попадает в выборку', sInt.some((x) => x.agent_id === 'T_ETH_okx' && x.internal_realized === -2.5 && x.realized === null));
check('внутренняя группа неизвестна → null', sInt.find((x) => x.agent_id === 'T_SOL_okx').internal_realized === null);

// --- группы точки: итого = внутренняя + внешние; неизвестная часть → null
const g = groupsOf(10, 0, 93.3, 36.7);
check('группы: внутренняя R=10 U=0', g.internal.r === 10 && g.internal.u === 0 && g.internal.total === 10);
check('группы: внешние R+U', Math.abs(g.external.total - 130) < 1e-9);
check('группы: итого R = внутр. + внеш., U = внутр. + внеш., R+U', Math.abs(g.total.r - 103.3) < 1e-9 && Math.abs(g.total.u - 36.7) < 1e-9 && Math.abs(g.total.total - 140) < 1e-9);
const gp = groupsOf(10, 0, 93.3, null);
check('группы: U внешних неизвестно → U итого и R+U итого = null (частичной суммы нет), R известен', gp.total.r !== null && gp.total.u === null && gp.total.total === null && gp.external.total === null);
const gn = groupsOf(null, null, 5, 1);
check('группы: внутренняя неизвестна → итого null, внешние как есть', gn.total.r === null && gn.total.total === null && gn.external.total === 6);

// --- DTO агента: группы в точках
const dtoG = buildPnlHistoryDto('T_BTC_binance', [
  { ts_ms: 1000, realized_usdt: 93.3, unrealized_usdt: 36.7, internal_realized_usdt: 10, internal_unrealized_usdt: 0 },
  { ts_ms: 2000, realized_usdt: null, unrealized_usdt: null, internal_realized_usdt: 11, internal_unrealized_usdt: 0 },
], 24, 5000);
check('DTO агента: группы в точке, старые поля r/u/total (внешние) сохранены', dtoG.points[0].groups.total.total !== null && dtoG.points[0].r === 93.3 && dtoG.points[0].total !== null);
check('DTO агента: точка только с внутренней группой не пропадает', dtoG.points.length === 2 && dtoG.points[1].groups.internal.r === 11 && dtoG.points[1].groups.external.r === null && dtoG.points[1].groups.total.r === null);

// --- итоги ledger для графика вкладки «PnL»
const grp = (r, u) => ({ has_data: r !== null, u_has_data: u !== null, r, u, r_plus_u: (r !== null && u !== null) ? r + u : null });
const tot = totalsSamples({
  all: { internal: grp(10, 0), external: grp(93.3, 36.7), total: grp(103.3, 36.7), agent_count: 61, incomplete_agents: 4 },
  translator: { internal: grp(8, 0), external: grp(90, 30), total: grp(98, 30), agent_count: 45, incomplete_agents: 3 },
  arbitrageur: null,
}, 1700000000000);
check('итоги: записи по охватам all и translator, арбитражёров без итога нет', tot.length === 2 && tot[0].scope === 'all' && tot[1].scope === 'translator');
check('итоги: части переписаны как есть, счётчики агентов', tot[0].internal_r === 10 && tot[0].internal_u === 0 && tot[0].external_r === 93.3 && tot[0].external_u === 36.7 && tot[0].agents === 61 && tot[0].incomplete === 4);
check('итоги: ledger недоступен (null) → ничего не пишем', totalsSamples(null, 1).length === 0);
const totU = totalsSamples({ all: { internal: grp(1, 0), external: grp(2, null), total: grp(3, null), agent_count: 2, incomplete_agents: 2 } }, 5);
check('итоги: U внешних неизвестно → null, R пишется', totU[0].external_r === 2 && totU[0].external_u === null && totU[0].internal_u === 0);
check('итоги: охват без единого значения пропускается', totalsSamples({ all: { internal: grp(null, null), external: grp(null, null), agent_count: 0, incomplete_agents: 0 } }, 5).length === 0);

const dtoT = buildTotalsHistoryDto('all', [
  { ts_ms: 2000, internal_r: 11, internal_u: 0, external_r: 94, external_u: 30, agents: 61, incomplete: 0 },
  { ts_ms: 1000, internal_r: 10, internal_u: 0, external_r: 93, external_u: 36, agents: 61, incomplete: 4 },
  { ts_ms: 3000, internal_r: null, internal_u: null, external_r: null, external_u: null },   // пустая запись пропускается
  { ts_ms: 'x', internal_r: 1 },
], 24, 10000000);
check('DTO итогов: две валидные точки по возрастанию времени', dtoT.points.length === 2 && dtoT.points[0].t === 1000 && dtoT.points[1].t === 2000 && dtoT.scope === 'all');
check('DTO итогов: группы и неполнота', dtoT.points[0].groups.total.total === 139 && dtoT.points[0].incomplete === 4 && dtoT.points[1].agents === 61);
check('DTO итогов: окно, начало, has_data', dtoT.window_hours === 24 && dtoT.first_ms === 1000 && dtoT.since_ms === 10000000 - 24 * 3600 * 1000 && dtoT.has_data === true);
check('DTO итогов: пустой ответ → has_data=false', buildTotalsHistoryDto('all', [], 24, 5).has_data === false);

if (failed) { console.error(failed + ' check(s) failed'); process.exit(1); }
console.log('all passed');
