import React from 'react';
import { render } from '@testing-library/react';
import HedgeModelThresholds from '../HedgeModelThresholds';
import { AgentPositionsTables } from '../VectorClearingLive';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

// ADR-088: действующие пороги хеджа и пороги модели рядом; браузер только показывает числа ledger.
const hm = (over = {}) => ({
  mode: 'shadow', computed: true, applied: false, reason: null, regime: 'noise',
  b_m: 18.9, b_t: 23.2, b_m_raw: 15.1, b_t_raw: 30.4, sz_est: 8.4, est_minutes: 62, transfer_threshold: null, transfer_target: null,
  transfer_threshold_raw: null, transfer_target_raw: null, mode_ceiling: 'shadow', ...over,
});
const translator = (over = {}) => ({
  agent_id: 'T_BTC_binance', agent_kind: 'translator', base: 'BTC', quote: 'USDT', venue: 'binance',
  c_total: -3.5, z_lim: 18, z_mkt: 18, hmodel: hm(), ...over,
});
const arbitrageur = (over = {}) => ({
  agent_id: 'A_BTC_binanceokx', agent_kind: 'arbitrageur', base: 'BTC', quote: 'USDT', venue: '',
  c_total: 0.2, z_lim: 4.5, z_mkt: 4.5,
  hmodel: hm({ regime: null, b_m: null, b_t: null, b_m_raw: null, b_t_raw: null, transfer_threshold: 41.6, transfer_target: 0,
               transfer_threshold_raw: 44.2, transfer_target_raw: 0 }), ...over,
});

const text = (el) => (el ? el.textContent.replace(/\s+/g, ' ').trim() : null);

test('переводчик: действующие 18 / 18 и модельные 18,9 / 23,2 стоят рядом, режим теневой', () => {
  const { container } = render(<HedgeModelThresholds translators={[translator()]} arbitrageurs={[]} />);
  expect(text(container.querySelector('[data-testid="hm-mode"]'))).toBe('теневой');
  const row = container.querySelector('[data-testid="hm-row-T_BTC_binance"]');
  expect(text(row.querySelector('[data-testid="hm-rule"]'))).toBe('18 / 18');
  expect(text(row.querySelector('[data-testid="hm-model"]'))).toBe('18,9 / 23,2');
  expect(text(row.querySelector('[data-testid="hm-state"]'))).toBe('считается, не применяется');
  expect(text(container.querySelector('[data-testid="hmodel-explain"]'))).toMatch(/на заявки не влияют/);
});

test('сырой расчёт показан отдельным столбцом: переводчик — b_м / b_т, связка — B / b; нули не теряются', () => {
  const { container } = render(<HedgeModelThresholds translators={[translator()]} arbitrageurs={[arbitrageur()]} />);
  expect(text(container.querySelector('[data-testid="hm-row-T_BTC_binance"] [data-testid="hm-raw"]'))).toBe('15,1 / 30,4');
  expect(text(container.querySelector('[data-testid="hm-row-A_BTC_binanceokx"] [data-testid="hm-raw"]'))).toBe('44,2 / 0');
  expect(container.querySelector('[data-testid="hm-raw-head"]')).not.toBeNull();
});

test('потолок окружения показан рядом с эффективным режимом; без него подписи нет', () => {
  const on = render(<HedgeModelThresholds translators={[translator({ hmodel: hm({ mode_ceiling: 'on' }) })]} arbitrageurs={[]} />);
  expect(text(on.container.querySelector('[data-testid="hm-mode"]'))).toBe('теневой');
  expect(text(on.container.querySelector('[data-testid="hm-ceiling"]'))).toMatch(/потолок окружения: on/);
  const none = render(<HedgeModelThresholds translators={[translator({ hmodel: hm({ mode_ceiling: null }) })]} arbitrageurs={[]} />);
  expect(none.container.querySelector('[data-testid="hm-ceiling"]')).toBeNull();
});

test('арбитражёр: связка B / b в своём столбце, модельных мейкер/тейкер нет — прочерк, а не ноль', () => {
  const { container } = render(<HedgeModelThresholds translators={[]} arbitrageurs={[arbitrageur()]} />);
  const row = container.querySelector('[data-testid="hm-row-A_BTC_binanceokx"]');
  expect(text(row.querySelector('[data-testid="hm-transfer"]'))).toBe('41,6 / 0');   // цель 0 — законное значение, не прочерк
  expect(text(row.querySelector('[data-testid="hm-model"]'))).toBe('—');
});

test('не посчитано: показана причина, пороги модели — прочерк', () => {
  const t = translator({ hmodel: hm({ computed: false, reason: 'sigma_stale', b_m: null, b_t: null }) });
  const { container } = render(<HedgeModelThresholds translators={[t]} arbitrageurs={[]} />);
  const row = container.querySelector('[data-testid="hm-row-T_BTC_binance"]');
  expect(text(row.querySelector('[data-testid="hm-state"]'))).toBe('волатильность устарела');
  expect(text(row.querySelector('[data-testid="hm-model"]'))).toBe('—');
});

test('нумерарий (A_USDT): причина по-русски', () => {
  const a = arbitrageur({ agent_id: 'A_USDT_binanceokx', z_lim: 37.8, z_mkt: 37.8, hmodel: hm({ computed: false, reason: 'numeraire', regime: null, b_m: null, b_t: null }) });
  const { container } = render(<HedgeModelThresholds translators={[]} arbitrageurs={[a]} />);
  expect(text(container.querySelector('[data-testid="hm-row-A_USDT_binanceokx"] [data-testid="hm-state"]'))).toBe('номинал: модели нет');
});

test('рабочий режим: пометка «применены к заявкам»', () => {
  const t = translator({ hmodel: hm({ mode: 'on', applied: true }) });
  const { container } = render(<HedgeModelThresholds translators={[t]} arbitrageurs={[]} />);
  expect(text(container.querySelector('[data-testid="hm-mode"]'))).toBe('рабочий');
  expect(text(container.querySelector('[data-testid="hm-state"]'))).toBe('применены к заявкам');
});

test('модели нет ни у кого (режим off): одна строка-пояснение, таблицы нет', () => {
  const { container } = render(<HedgeModelThresholds translators={[translator({ hmodel: null })]} arbitrageurs={[]} />);
  expect(container.querySelector('[data-testid="hmodel-off"]')).not.toBeNull();
  expect(container.querySelector('table')).toBeNull();
});

test('нет агентов — блока нет', () => {
  const { container } = render(<HedgeModelThresholds translators={[]} arbitrageurs={[]} />);
  expect(container.firstChild).toBeNull();
});

test('в таблице позиций блок порогов показан, когда есть агенты', () => {
  const { container } = render(<AgentPositionsTables agentRows={[translator()]} title="t" />);
  expect(container.querySelector('[data-testid="hmodel-block"]')).not.toBeNull();
});
