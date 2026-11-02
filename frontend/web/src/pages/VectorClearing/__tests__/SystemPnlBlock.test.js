import React from 'react';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import axios from 'axios';
import SystemPnlBlock from '../SystemPnlBlock';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));

const dto = (over = {}) => ({
  window_hours: 24, baseline_ms: 1792660000000, has_data: true,
  current: {
    trading: 200, revaluation: 3000, mtm_change: 3200, mtm: 1803200, mtm0: 1800000, skipped: [],
    by_currency: [
      { currency: 'BTC', q0: 10, q: 11, dq: 1, m0: 80000, m: 80300, trading: 80300, revaluation: 3000 },
      { currency: 'USDT', q0: 1000000, q: 919900, dq: -80100, m0: 1, m: 1, trading: -80100, revaluation: 0 },
    ],
  },
  points: [{ t: 1000, trading: 0, revaluation: 0, mtm_change: 0 }, { t: 61000, trading: 100, revaluation: 1000, mtm_change: 1100 }, { t: 121000, trading: 200, revaluation: 3000, mtm_change: 3200 }],
  ...over,
});

afterEach(() => { jest.clearAllMocks(); });

test('показывает торговый результат, переоценку, итог, базу и три линии', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-trading')).toBeTruthy());
  expect(screen.getByTestId('sys-trading').textContent).toMatch(/\+200\.0/);
  expect(screen.getByTestId('sys-reval').textContent).toMatch(/3.?000/);
  expect(screen.getByTestId('sys-total').textContent).toMatch(/3.?200/);
  expect(screen.getByTestId('sys-base').textContent).toMatch(/база:/);
  expect(screen.getByTestId('sys-line-trading')).toBeTruthy();
  expect(screen.getByTestId('sys-line-reval')).toBeTruthy();
  expect(screen.getByTestId('sys-line-total')).toBeTruthy();
  expect(axios.get.mock.calls[0][1].params).toEqual({ hours: 24 });
});

test('таблица по валютам: остаток на базе, сейчас, изменение, вклады', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  const { container } = render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-table')).toBeTruthy());
  const rows = container.querySelectorAll('[data-testid="sys-table"] tbody tr');
  expect(rows.length).toBe(2);
  expect(rows[0].textContent).toMatch(/BTC/);
  expect(rows[1].textContent).toMatch(/USDT/);
});

test('нет базы: понятное сообщение, графика нет', async () => {
  axios.get.mockResolvedValue({ data: { window_hours: 24, baseline_ms: null, current: null, points: [], has_data: false } });
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-empty')).toBeTruthy());
  expect(screen.getByTestId('sys-empty').textContent).toMatch(/Учёт ещё не начат/);
  expect(screen.queryByTestId('sys-svg')).toBeNull();
});

test('валюты без цены перечислены; ошибка загрузки показана', async () => {
  axios.get.mockResolvedValueOnce({ data: dto({ current: { ...dto().current, skipped: ['SOL'] } }) });
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-base')).toBeTruthy());
  expect(screen.getByTestId('sys-base').textContent).toMatch(/без цены: SOL/);
});

test('ошибка загрузки не роняет блок', async () => {
  axios.get.mockRejectedValue(new Error('boom'));
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-error')).toBeTruthy());
});

test('смена окна перезапрашивает ряд', async () => {
  axios.get.mockResolvedValue({ data: dto() });
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-trading')).toBeTruthy());
  fireEvent.click(screen.getByTestId('sys-window-168'));
  await waitFor(() => expect(axios.get.mock.calls.some((c) => c[1].params.hours === 168)).toBe(true));
});

test('остатки недоступны (ledger): показана ошибка, а не «учёт не начат»; записанный ряд остаётся', async () => {
  axios.get.mockResolvedValue({ data: { window_hours: 24, baseline_ms: 1792660000000, current: null, current_error: 'ledger_unavailable',
    points: [{ t: 1000, trading: 0, revaluation: 0, mtm_change: 0 }, { t: 61000, trading: 5, revaluation: 7, mtm_change: 12 }], has_data: true } });
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-current-error')).toBeTruthy());
  expect(screen.getByTestId('sys-current-error').textContent).toMatch(/ledger не отвечает/);
  expect(screen.queryByTestId('sys-empty')).toBeNull();
});

test('ledger перезапущен, новая база ещё не задана: объяснено, что остатки обнулились, а не «нет данных о ценах»', async () => {
  axios.get.mockResolvedValue({ data: { window_hours: 24, baseline_ms: 1792660000000, current: null, current_error: 'ledger_restarted',
    points: [{ t: 1000, trading: 0, revaluation: 0, mtm_change: 0 }], has_data: true } });
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-current-error')).toBeTruthy());
  expect(screen.getByTestId('sys-current-error').textContent).toMatch(/Ledger перезапущен/);
  expect(screen.getByTestId('sys-current-error').textContent).not.toMatch(/нет данных о ценах/);
});

test('перенос накопленного через перезапуск ledger показан', async () => {
  axios.get.mockResolvedValue({ data: dto({ current: { ...dto().current, carry: { trading: 50, revaluation: 20 } } }) });
  render(<SystemPnlBlock pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('sys-carry')).toBeTruthy());
  expect(screen.getByTestId('sys-carry').textContent).toMatch(/до перезапуска ledger/);
});
