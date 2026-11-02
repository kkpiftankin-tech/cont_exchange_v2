import React from 'react';
import { render } from '@testing-library/react';
import { AgentPositionsTables } from '../VectorClearingLive';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

// Панель «Живые позиции»: заработок по сделкам у переводчиков и арбитражёров, позиции арбитражёров отдельно (базис и открытая
// позиция — не результат), строка общего заработка и пояснение простыми словами.
const translator = {
  agent_id: 'T_ETH_binance', agent_kind: 'translator', base: 'ETH', quote: 'USDT', venue: 'binance', hasDelta: true,
  pos_base_before: 0.5, delta_base: -0.01, pos_base: 0.49, pos_quote_before: -1200, delta_quote: 24, pos_quote: -1176,
  realized_usdt: 3.2, unrealized_usdt: 1.1, pnl_usdt: 4.3, trade_fills: 12,
};
const arbitrageur = (over = {}) => ({
  agent_id: 'A_ETH_binanceokx', agent_kind: 'arbitrageur', base: 'ETH', quote: 'USDT', venue: '', hasDelta: true,
  arb: { src: 'binance', dst: 'okx', unit: 'ETH', leg_src: 1.7579, leg_dst: -1.7579, bleg_src: 1.7, bleg_dst: -1.7, dleg_src: 0.0579, dleg_dst: -0.0579 },
  pnl_usdt: null, realized_usdt: null, unrealized_usdt: null, trade_fills: null, ...over,
});

test('арбитражёр: показаны базис и открытая позиция со знаками (это позиция, не результат)', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator, arbitrageur({ basis_mtm_usdt: 0.2461, open_position_usdt: -15.5 })]} title="t" />);
  const cell = container.querySelector('[data-testid="arb-basis"]');
  expect(cell).not.toBeNull();
  expect(cell.textContent).toMatch(/базис\s*\+0\.2461/);
  expect(cell.textContent).toMatch(/открытая позиция\s*-15\.5000/);
  expect(cell.getAttribute('title')).toMatch(/риск изменения цены, а не убыток/);
});

test('арбитражёр со сделками: сверху заработок R / U, под ним позиция', () => {
  const { container } = render(<AgentPositionsTables agentRows={[arbitrageur({ realized_usdt: 10, unrealized_usdt: -4, pnl_usdt: 6, trade_fills: 30, basis_mtm_usdt: 0.5, open_position_usdt: -8000 })]} title="t" />);
  const cell = container.querySelector('[data-testid="arb-pnl"]');
  expect(cell.textContent).toMatch(/\+10\.0000.*\/ -4\.0000/);
  expect(cell.textContent).toMatch(/открытая позиция\s*-8000\.00/);
});

test('арбитражёр без цен и без сделок: прочерк, ничего не выдумываем', () => {
  const { container } = render(<AgentPositionsTables agentRows={[arbitrageur({ basis_mtm_usdt: null, open_position_usdt: null })]} title="t" />);
  expect(container.querySelector('[data-testid="arb-basis"]')).toBeNull();
  expect(container.querySelector('[data-testid="arb-pnl"]')).toBeNull();
});

test('под таблицей арбитражёров простыми словами: заработок по сделкам, базис, открытая позиция — риск, а не убыток', () => {
  const { container } = render(<AgentPositionsTables agentRows={[arbitrageur({ basis_mtm_usdt: -0.35, open_position_usdt: 100 })]} title="t" />);
  const note = container.querySelector('[data-testid="arb-explain"]');
  expect(note).not.toBeNull();
  expect(note.textContent).toMatch(/выходит на внешнюю биржу/);
  expect(note.textContent).toMatch(/базис/);
  expect(note.textContent).toMatch(/открытая позиция/);
  expect(note.textContent).toMatch(/риск изменения цены, а не убыток/);
  expect(note.textContent).toMatch(/счёт дома/);
});

// Итоги заработка считает ledger (GetAgentPnl) и приходят в пропе pnlTotals; панель их только показывает (ADR-087 п. 10).
const grp = (r, u, total) => ({ has_data: true, u_has_data: true, r, u, r_plus_u: total === undefined ? r + u : total });
const totals = (over = {}) => ({
  translator: { external: grp(3.2, 1.1), internal: grp(0, 0), total: grp(3.2, 1.1) },
  arbitrageur: { external: grp(10, -4), internal: grp(0, 0), total: grp(10, -4) },
  all: { external: grp(13.2, -2.9), internal: grp(0, 0), total: grp(13.2, -2.9) },
  ...over,
});

test('строка общего заработка: переводчики, арбитражёры и «Вместе» берутся из итогов ledger, позиции арбитражёров отдельно', () => {
  const rows = [translator,
    arbitrageur({ realized_usdt: 10, unrealized_usdt: -4, pnl_usdt: 6, trade_fills: 30, basis_mtm_usdt: 0.25, open_position_usdt: -8000 }),
    arbitrageur({ agent_id: 'A_BTC_krakenokx', basis_mtm_usdt: -0.5, open_position_usdt: -2000 })];
  const { container } = render(<AgentPositionsTables agentRows={rows} pnlTotals={totals()} title="t" />);
  const t = container.querySelector('[data-testid="trade-totals"]').textContent;
  expect(t).toMatch(/Общий заработок на внешних биржах \(из ledger/);
  expect(t).toMatch(/Переводчики \(1\): R \+3\.2000 \/ U \+1\.1000/);
  expect(t).toMatch(/Арбитражёры \(2\): R \+10\.0000 \/ U -4\.0000/);
  const all = container.querySelector('[data-testid="total-earnings"]').textContent;
  expect(all).toMatch(/Вместе: R \+13\.2000 \/ U -2\.9000 · итого R\+U \+10\.3000/);
  const pos = container.querySelector('[data-testid="arb-pos-total"]').textContent;
  expect(pos).toMatch(/не заработок/);
  expect(pos).toMatch(/базис -0\.2500/);
  expect(pos).toMatch(/открытая позиция -10000\.00/);
});

test('панель не складывает: итоги показываются ровно как пришли из ledger, даже если они не равны сумме строк', () => {
  // В строках R = 3.2 и 10, а ledger прислал другие итоги: если бы панель суммировала сама, увидели бы +13.2000.
  const rows = [translator, arbitrageur({ realized_usdt: 10, unrealized_usdt: -4, pnl_usdt: 6 })];
  const odd = totals({ all: { external: grp(777, 1, 999), internal: grp(0, 0), total: grp(0, 0) } });
  const { container } = render(<AgentPositionsTables agentRows={rows} pnlTotals={odd} title="t" />);
  const all = container.querySelector('[data-testid="total-earnings"]').textContent;
  expect(all).toMatch(/Вместе: R \+777\.00 \/ U \+1\.0000 · итого R\+U \+999\.00/);
  expect(all).not.toMatch(/13\.2000/);
});

test('итоги недоступны (ledger не ответил) — «нет данных от ledger», а не нули', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator]} pnlTotals={null} title="t" />);
  expect(container.querySelector('[data-testid="pnl-totals-missing"]')).not.toBeNull();
  expect(container.querySelector('[data-testid="total-earnings"]')).toBeNull();
});

test('нет данных у вида агентов — прочерк в его итоге, остальные виды показаны', () => {
  const noArb = totals({ arbitrageur: { external: { has_data: false, u_has_data: false, r: null, u: null, r_plus_u: null } } });
  const { container } = render(<AgentPositionsTables agentRows={[translator, arbitrageur()]} pnlTotals={noArb} title="t" />);
  const t = container.querySelector('[data-testid="trade-totals"]').textContent;
  expect(t).toMatch(/Переводчики \(1\): R \+3\.2000/);
  expect(t).toMatch(/Арбитражёры \(1\): R —/);
});

test('подпись итога: число агентов из ledger и сколько без данных (в сумму не входят)', () => {
  const t = totals({
    translator: { external: grp(3.2, 1.1), internal: grp(0, 0), total: grp(3.2, 1.1), agent_count: 17, incomplete_agents: 4 },
    arbitrageur: { external: grp(0, 0), internal: grp(0, 0), total: grp(0, 0), agent_count: 30, incomplete_agents: 0 },
  });
  const { container } = render(<AgentPositionsTables agentRows={[translator, arbitrageur()]} pnlTotals={t} title="t" />);
  const text = container.querySelector('[data-testid="trade-totals"]').textContent;
  expect(text).toMatch(/Переводчики \(17, без данных: 4\): R \+3\.2000/);
  expect(text).toMatch(/Арбитражёры \(30\): R/);
});

test('большая открытая позиция арбитражёра не меняет «Вместе» (позиция — не заработок)', () => {
  const a = arbitrageur({ realized_usdt: 1, unrealized_usdt: 0, pnl_usdt: 1, basis_mtm_usdt: 0, open_position_usdt: 0 });
  const b = arbitrageur({ realized_usdt: 1, unrealized_usdt: 0, pnl_usdt: 1, basis_mtm_usdt: 0, open_position_usdt: -14000 });
  const tt = (rows) => render(<AgentPositionsTables agentRows={rows} pnlTotals={totals()} title="t" />).container.querySelector('[data-testid="total-earnings"]').textContent;
  expect(tt([a])).toBe(tt([b]));
});

test('в строке агента R и U показываются без запасного пересчёта: нет R — пусто', () => {
  const withFallbackBait = { ...translator, realized_usdt: null, unrealized_usdt: null, pnl_usdt: 4.3 };
  const { container } = render(<AgentPositionsTables agentRows={[withFallbackBait]} pnlTotals={totals()} title="t" />);
  expect(container.textContent).not.toMatch(/\+4\.3000/);
});

test('переводчик по-прежнему показывает realized / unrealized', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator]} title="t" />);
  expect(container.textContent).toMatch(/\+3\.2000/);
});
