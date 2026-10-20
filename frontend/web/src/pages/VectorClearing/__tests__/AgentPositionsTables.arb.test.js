import React from 'react';
import { render } from '@testing-library/react';
import { AgentPositionsTables } from '../VectorClearingLive';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

// Панель «Живые позиции»: заработок арбитражёров (базис позиции, а не «—» и не нулевые сделки) и пояснение простыми словами.
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

test('арбитражёр: вместо прочерка показан базис позиции со знаком и подписью', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator, arbitrageur({ basis_mtm_usdt: 0.2461 })]} title="t" />);
  const cell = container.querySelector('[data-testid="arb-basis"]');
  expect(cell).not.toBeNull();
  expect(cell.textContent).toMatch(/\+0\.2461.*базис/);
  expect(cell.getAttribute('title')).toMatch(/Наружу арбитражёр не торгует/);
});

test('арбитражёр без цен: базис не выдумывается — прочерк', () => {
  const { container } = render(<AgentPositionsTables agentRows={[arbitrageur({ basis_mtm_usdt: null })]} title="t" />);
  expect(container.querySelector('[data-testid="arb-basis"]')).toBeNull();
});

test('под таблицей арбитражёров простыми словами объяснено, почему нет заработка по сделкам', () => {
  const { container } = render(<AgentPositionsTables agentRows={[arbitrageur({ basis_mtm_usdt: -0.35 })]} title="t" />);
  const note = container.querySelector('[data-testid="arb-explain"]');
  expect(note).not.toBeNull();
  expect(note.textContent).toMatch(/наружу не торгует/);
  expect(note.textContent).toMatch(/тариф перевоза/);
  expect(note.textContent).toMatch(/счёт дома/);
});

test('итоги: арбитражёры отдельно как базис позиций, без смешанной суммы «Вместе»', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator, arbitrageur({ basis_mtm_usdt: 0.25 }), arbitrageur({ agent_id: 'A_BTC_krakenokx', basis_mtm_usdt: -0.5 })]} title="t" />);
  const totals = container.querySelector('.vc-trade-totals').textContent;
  expect(totals).toMatch(/Переводчики \(1\)/);
  expect(totals).toMatch(/Арбитражёры, базис позиций \(2\).*-0\.2500/);
  expect(totals).not.toMatch(/Вместе/);
});

test('переводчик по-прежнему показывает realized / unrealized', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator]} title="t" />);
  expect(container.textContent).toMatch(/\+3\.2000/);
});
