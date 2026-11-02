import React from 'react';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import axios from 'axios';
import AgentPnlTable from '../AgentPnlTable';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));

const grp = (r, u, total) => ({ has_data: true, u_has_data: true, r, u, r_plus_u: total === undefined ? r + u : total });
const NO = { has_data: false, u_has_data: false, r: null, u: null, r_plus_u: null };

const dto = (over = {}) => ({
  currency: 'USDT',
  enabled: true,
  agents: [
    {
      agent_id: 'T_BTC_binance', agent_kind: 'translator', asset: 'BTC', quote: 'USDT', venues: ['binance'],
      internal: grp(60, 40), external: grp(40, 0), total: grp(100, 40), mark_age_ms: 1500, ext_fills: 3, since_ms: 1792420000000,
    },
    {
      agent_id: 'T_ETH_okx', agent_kind: 'translator', asset: 'ETH', quote: 'USDT', venues: ['okx'],
      internal: NO, external: NO, total: NO, mark_age_ms: null, ext_fills: 0, since_ms: null,
    },
    {
      agent_id: 'A_BTC_binancekraken', agent_kind: 'arbitrageur', asset: 'BTC', quote: 'USDT', venues: ['binance', 'kraken'],
      internal: grp(9.99, 0.005), external: grp(0, 0), total: grp(9.99, 0.005), mark_age_ms: 900, ext_fills: 0, since_ms: 1792420000000,
    },
  ],
  totals: { internal: grp(69.99, 40.005), external: grp(40, 0), total: grp(110, 40), agent_count: 3, incomplete_agents: 1 },
  generatedAt: '2026-10-28T12:00:00.000Z',
  ...over,
});

afterEach(() => { jest.clearAllMocks(); });

const txt = (id) => screen.getByTestId(id).textContent.replace(/\s/g, '');

test('запрашивает маршрут agent-pnl и рисует три группы столбцов', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-table')).toBeTruthy());
  expect(axios.get.mock.calls[0][0]).toMatch(/\/vector-clearing\/agent-pnl$/);
  expect(screen.getByTestId('agent-pnl-head-internal').textContent).toBe('Внутренняя биржа CE');
  expect(screen.getByTestId('agent-pnl-head-external').textContent).toBe('Внешние биржи');
  expect(screen.getByTestId('agent-pnl-head-total').textContent).toBe('Итого');
  // В каждой группе три столбца R, U, R+U (во второй строке шапки — девять подзаголовков).
  const heads = Array.from(document.querySelectorAll('thead tr:nth-child(2) th')).map((th) => th.textContent);
  expect(heads).toEqual(['R', 'U', 'R+U', 'R', 'U', 'R+U', 'R', 'U', 'R+U']);
});

test('показывает числа агента по группам со знаком', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-T_BTC_binance')).toBeTruthy());
  expect(txt('agent-pnl-T_BTC_binance-internal-r')).toBe('+60,00');
  expect(txt('agent-pnl-T_BTC_binance-internal-u')).toBe('+40,00');
  expect(txt('agent-pnl-T_BTC_binance-internal-r_plus_u')).toBe('+100,00');
  expect(txt('agent-pnl-T_BTC_binance-external-r')).toBe('+40,00');
  expect(txt('agent-pnl-T_BTC_binance-external-u')).toBe('0,00');
  expect(txt('agent-pnl-T_BTC_binance-total-r_plus_u')).toBe('+140,00');
});

test('фронт не считает: R+U, итого и строка «все агенты» показываются ровно как пришли', async () => {
  // Нарочно несогласованные числа: если бы компонент пересчитывал, увидели бы 30,00 вместо 7,77.
  const odd = dto({
    agents: [{ ...dto().agents[0], total: { has_data: true, u_has_data: true, r: 10, u: 20, r_plus_u: 7.77 } }],
    totals: { internal: grp(1, 1), external: grp(1, 1), total: { has_data: true, u_has_data: true, r: 1, u: 2, r_plus_u: 123.45 }, agent_count: 1, incomplete_agents: 0 },
  });
  axios.get.mockResolvedValue({ data: odd });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-T_BTC_binance')).toBeTruthy());
  expect(txt('agent-pnl-T_BTC_binance-total-r_plus_u')).toBe('+7,77');
  expect(txt('agent-pnl-all-total-r_plus_u')).toBe('+123,45');
});

test('нет данных — прочерк, а не ноль; подсказка объясняет причину', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-T_ETH_okx')).toBeTruthy());
  expect(screen.getByTestId('agent-pnl-T_ETH_okx-internal-r').textContent).toBe('—');
  expect(screen.getByTestId('agent-pnl-T_ETH_okx-total-r_plus_u').textContent).toBe('—');
  expect(screen.getByTestId('agent-pnl-T_ETH_okx-internal-r').getAttribute('title')).toMatch(/нет данных/);
});

test('R известен, U — нет: R показан, U и R+U — прочерк с объяснением (после перезапуска, пока нет справедливой цены)', async () => {
  const partial = { has_data: true, u_has_data: false, r: 93.33, u: null, r_plus_u: null };
  const d = dto({ agents: [{ ...dto().agents[0], external: partial, total: partial }] });
  axios.get.mockResolvedValue({ data: d });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-T_BTC_binance')).toBeTruthy());
  expect(txt('agent-pnl-T_BTC_binance-external-r')).toBe('+93,33');
  expect(screen.getByTestId('agent-pnl-T_BTC_binance-external-u').textContent).toBe('—');
  expect(screen.getByTestId('agent-pnl-T_BTC_binance-external-u').getAttribute('title')).toMatch(/справедливой цены/);
  expect(screen.getByTestId('agent-pnl-T_BTC_binance-total-r_plus_u').textContent).toBe('—');
});

test('строка «все агенты»: число агентов и сколько без данных', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-total-row')).toBeTruthy());
  expect(screen.getByTestId('agent-pnl-total-row').textContent).toMatch(/Все агенты \(3, без данных: 1\)/);
  expect(txt('agent-pnl-all-internal-r')).toBe('+69,99');
  expect(txt('agent-pnl-all-total-r_plus_u')).toBe('+150,00');
});

test('связка: подпись маршрута площадок', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-A_BTC_binancekraken')).toBeTruthy());
  expect(screen.getByTestId('agent-pnl-row-A_BTC_binancekraken').textContent).toMatch(/BTC · binance ↔ kraken/);
});

test('ledger недоступен (502) — понятное сообщение, прежние данные остаются', async () => {
  axios.get.mockResolvedValueOnce({ data: dto() }).mockRejectedValue({ response: { status: 502 } });
  render(<AgentPnlTable pollMs={30} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-error').textContent).toMatch(/ledger недоступен/));
  expect(screen.getByTestId('agent-pnl-table')).toBeTruthy();
});

test('учёт выключен — сообщение вместо таблицы', async () => {
  axios.get.mockResolvedValue({ data: dto({ enabled: false, agents: [] }) });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-off')).toBeTruthy());
  expect(screen.queryByTestId('agent-pnl-table')).toBeNull();
});

test('нет агентов — пустое состояние', async () => {
  axios.get.mockResolvedValue({ data: dto({ agents: [], totals: dto().totals }) });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-empty')).toBeTruthy());
});

test('клик по строке выбирает агента для графика, повторный клик снимает выбор; без обработчика строки не кликабельны', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  const onSelect = jest.fn();
  const { rerender } = render(<AgentPnlTable pollMs={600000} selectedId={null} onSelectAgent={onSelect} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-T_BTC_binance')).toBeTruthy());
  fireEvent.click(screen.getByTestId('agent-pnl-row-T_BTC_binance'));
  expect(onSelect).toHaveBeenLastCalledWith('T_BTC_binance');
  rerender(<AgentPnlTable pollMs={600000} selectedId="T_BTC_binance" onSelectAgent={onSelect} />);
  expect(screen.getByTestId('agent-pnl-row-T_BTC_binance').className).toMatch(/agent-pnl-row-selected/);
  fireEvent.click(screen.getByTestId('agent-pnl-row-T_BTC_binance'));
  expect(onSelect).toHaveBeenLastCalledWith(null);
  expect(screen.getByTestId('agent-pnl-row-T_ETH_okx').className).not.toMatch(/selected/);
});

test('без onSelectAgent строки обычные (компонент по-прежнему работает отдельно)', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<AgentPnlTable pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-T_BTC_binance')).toBeTruthy());
  expect(screen.getByTestId('agent-pnl-row-T_BTC_binance').className).toBe('');
});

test('строка выбирается и с клавиатуры: Enter и пробел, фокус через Tab', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  const onSelect = jest.fn();
  render(<AgentPnlTable pollMs={600000} selectedId={null} onSelectAgent={onSelect} />);
  await waitFor(() => expect(screen.getByTestId('agent-pnl-row-T_BTC_binance')).toBeTruthy());
  const row = screen.getByTestId('agent-pnl-row-T_BTC_binance');
  expect(row.getAttribute('tabindex')).toBe('0');
  fireEvent.keyDown(row, { key: 'Enter' });
  expect(onSelect).toHaveBeenLastCalledWith('T_BTC_binance');
  fireEvent.keyDown(row, { key: ' ' });
  expect(onSelect).toHaveBeenCalledTimes(2);
  fireEvent.keyDown(row, { key: 'a' });
  expect(onSelect).toHaveBeenCalledTimes(2);
});
