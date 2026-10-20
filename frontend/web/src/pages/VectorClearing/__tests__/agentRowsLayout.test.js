import React from 'react';
import { render } from '@testing-library/react';
import { groupTranslators, groupArbitrageurs, isIdleAgent, mergeStableRows } from '../agentRowsLayout';
import { AgentPositionsTables } from '../VectorClearingLive';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

// Панель «Живые позиции»: группы по биржам, фиксированный порядок, все агенты сразу (владелец, 2026-10-20).
const T = (id, base, quote, venue, extra = {}) => ({
  agent_id: id, agent_kind: 'translator', base, quote, venue, hasDelta: false,
  pos_base: 0, pos_quote: 0, pos_base_before: 0, pos_quote_before: 0, delta_base: 0, delta_quote: 0, c_total: 0, c_position: 0, ...extra,
});
const A = (id, asset, src, dst, extra = {}) => ({
  agent_id: id, agent_kind: 'arbitrageur', base: asset, quote: 'USDT', venue: '', hasDelta: false,
  arb: { src, dst, unit: asset, leg_src: 0, leg_dst: 0, bleg_src: 0, bleg_dst: 0, dleg_src: 0, dleg_dst: 0 }, ...extra,
});

const rows = [
  T('T_SOL_okx', 'SOL', 'USDT', 'okx'),
  T('T_ETH_BTC_binance', 'ETH', 'BTC', 'binance'),
  T('T_BTC_okx', 'BTC', 'USDT', 'okx', { hasDelta: true, c_total: 3, pos_base: -0.03 }),
  T('T_ETH_binance', 'ETH', 'USDT', 'binance'),
  T('T_BTC_binance', 'BTC', 'USDT', 'binance', { c_total: 1.2, pos_base: -0.01 }),
  A('A_ETH_binanceokx', 'ETH', 'binance', 'okx'),
  A('A_BTC_binancekraken', 'BTC', 'binance', 'kraken', { hasDelta: true }),
  A('A_BTC_binanceokx', 'BTC', 'binance', 'okx'),
  A('A_USDT_binanceokx', 'USDT', 'binance', 'okx'),
  A('A_BTC_krakenokx', 'BTC', 'kraken', 'okx'),
];

test('переводчики сгруппированы по биржам: биржи по алфавиту, внутри BTC → ETH → кросс-пара', () => {
  const g = groupTranslators(rows);
  expect(g.map((x) => x.venue)).toEqual(['binance', 'okx']);
  expect(g[0].rows.map((r) => r.agent_id)).toEqual(['T_BTC_binance', 'T_ETH_binance', 'T_ETH_BTC_binance']);
  expect(g[1].rows.map((r) => r.agent_id)).toEqual(['T_BTC_okx', 'T_SOL_okx']);
});

test('арбитражёры сгруппированы по паре бирж «источник ↔ приёмник»: один агент — в одной группе', () => {
  const g = groupArbitrageurs(rows);
  expect(g.map((x) => x.key)).toEqual(['binance ↔ kraken', 'binance ↔ okx', 'kraken ↔ okx']);
  expect(g[1].rows.map((r) => r.agent_id)).toEqual(['A_BTC_binanceokx', 'A_ETH_binanceokx', 'A_USDT_binanceokx']);   // USDT — последним
  expect(g.flatMap((x) => x.rows).length).toBe(5);
});

test('порядок не зависит ни от порядка во входе, ни от того, двигался ли агент в этом такте', () => {
  const seq = (arr) => [...groupTranslators(arr).flatMap((g) => g.rows), ...groupArbitrageurs(arr).flatMap((g) => g.rows)].map((r) => r.agent_id);
  const base = seq(rows);
  const reversed = seq([...rows].reverse());
  const flipped = seq(rows.map((r) => ({ ...r, hasDelta: !r.hasDelta })));                       // все «двигавшиеся» стали неподвижными и наоборот
  const shuffled = seq([rows[3], rows[7], rows[0], rows[9], rows[5], rows[1], rows[8], rows[2], rows[6], rows[4]]);
  expect(reversed).toEqual(base);
  expect(flipped).toEqual(base);
  expect(shuffled).toEqual(base);
});

test('агент без позиции и без дельты — «не действует» (приглушается), но остаётся в списке', () => {
  expect(isIdleAgent(T('T_SOL_okx', 'SOL', 'USDT', 'okx'))).toBe(true);
  expect(isIdleAgent(T('T_BTC_okx', 'BTC', 'USDT', 'okx', { c_total: 3 }))).toBe(false);
  expect(isIdleAgent(T('T_BTC_okx', 'BTC', 'USDT', 'okx', { hasDelta: true }))).toBe(false);
  expect(isIdleAgent(A('A_BTC_x', 'BTC', 'a', 'b'))).toBe(true);
  expect(isIdleAgent(A('A_BTC_x', 'BTC', 'a', 'b', { arb: { src: 'a', dst: 'b', leg_src: -0.5, leg_dst: 0.5 } }))).toBe(false);
});

test('mergeStableRows: пропавший из ответа агент остаётся (stale, без дельты), новые добавляются', () => {
  const prev = [T('T_BTC_okx', 'BTC', 'USDT', 'okx', { hasDelta: true, delta_base: -0.01, c_total: 3 }),
    A('A_BTC_krakenokx', 'BTC', 'kraken', 'okx', { hasDelta: true, arb: { src: 'kraken', dst: 'okx', leg_src: -1, leg_dst: 1, dleg_src: -0.5, dleg_dst: 0.5 } })];
  const next = [T('T_ETH_okx', 'ETH', 'USDT', 'okx')];
  const merged = mergeStableRows(prev, next);
  expect(merged.map((r) => r.agent_id).sort()).toEqual(['A_BTC_krakenokx', 'T_BTC_okx', 'T_ETH_okx']);
  const kept = merged.find((r) => r.agent_id === 'T_BTC_okx');
  expect(kept.stale).toBe(true);
  expect(kept.hasDelta).toBe(false);
  expect(kept.delta_base).toBe(0);
  expect(kept.c_total).toBe(3);                                                                       // последняя известная позиция сохранена
  const keptArb = merged.find((r) => r.agent_id === 'A_BTC_krakenokx');
  expect(keptArb.arb.dleg_src).toBe(0);
  expect(keptArb.arb.leg_dst).toBe(1);
  expect(merged.find((r) => r.agent_id === 'T_ETH_okx').stale).toBeFalsy();
  expect(mergeStableRows(null, next).length).toBe(1);                                                  // первый опрос
  const back = mergeStableRows(merged, [T('T_BTC_okx', 'BTC', 'USDT', 'okx')]);                       // агент вернулся — больше не stale
  expect(back.find((r) => r.agent_id === 'T_BTC_okx').stale).toBeFalsy();
});

const idsInDom = (container) => [...container.querySelectorAll('tr')].map((tr) => tr.textContent.slice(0, 40));

test('панель: заголовки групп по биржам и парам бирж, порядок строк одинаков при любом ходе клиринга', () => {
  const { container, rerender } = render(<AgentPositionsTables agentRows={rows} title="t" />);
  const heads = [...container.querySelectorAll('.vc-group-head')].map((n) => n.textContent);
  expect(heads).toEqual([
    'binance — переводчики: 3', 'okx — переводчики: 2',
    'binance ↔ kraken — арбитражёры: 1', 'binance ↔ okx — арбитражёры: 3', 'kraken ↔ okx — арбитражёры: 1',
  ]);
  const before = idsInDom(container);
  // следующий такт: двигались другие агенты, порядок во входе другой
  const next = [...rows].reverse().map((r, i) => ({ ...r, hasDelta: i % 2 === 0 }));
  rerender(<AgentPositionsTables agentRows={next} title="t" />);
  const after = idsInDom(container);
  const names = (arr) => arr.filter((s) => /^(T_|A_|binance|okx|kraken)/.test(s)).map((s) => s.split(' ')[0]);
  expect(names(after)).toEqual(names(before));
});

test('панель: не действующий агент показан сразу (приглушён), а устаревший — ещё слабее', () => {
  const list = [T('T_BTC_okx', 'BTC', 'USDT', 'okx', { c_total: 3 }), T('T_SOL_okx', 'SOL', 'USDT', 'okx'), T('T_ETH_okx', 'ETH', 'USDT', 'okx', { stale: true, c_total: 2 })];
  const { container } = render(<AgentPositionsTables agentRows={list} title="t" />);
  const opacityOf = (id) => { const tr = [...container.querySelectorAll('tr')].find((r) => r.textContent.startsWith(id)); return tr ? tr.style.opacity : null; };
  expect(opacityOf('T_BTC_okx')).toBe('');
  expect(opacityOf('T_SOL_okx')).toBe('0.6');
  expect(opacityOf('T_ETH_okx')).toBe('0.4');
});
