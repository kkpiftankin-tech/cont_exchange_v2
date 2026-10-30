import React from 'react';
import { render, screen, waitFor, fireEvent, act } from '@testing-library/react';
import axios from 'axios';
import PnlHistoryChart from '../PnlHistoryChart';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));

const g = (r, u) => ({ r, u, total: r !== null && u !== null ? r + u : null });
// Точка графика: три группы уже посчитаны BFF (внутренняя, внешние, итого), браузер только выбирает группу.
const pt = (t, int, ext, tot, extra = {}) => ({ t, groups: { internal: int, external: ext, total: tot }, agents: 61, incomplete: 0, ...extra });
const totalsDto = (points, scope = 'all') => ({ scope, window_hours: 24, since_ms: 0, first_ms: points.length ? points[0].t : null, points, has_data: points.length > 0 });
const POINTS = [
  pt(1000, g(1, 0), g(10, 2), g(11, 2)),
  pt(61000, g(2, 0), g(12, 3), g(14, 3)),
  pt(121000, g(2.5, 0), g(13.5, -1), g(16, -1), { incomplete: 4 }),
];

afterEach(() => { jest.clearAllMocks(); try { window.localStorage.clear(); } catch (e) { /* нет хранилища */ } });

test('по умолчанию — все агенты, группа «Итого», окно 24 ч: запрос итогов и три ряда', async () => {
  axios.get.mockResolvedValue({ data: totalsDto(POINTS) });
  render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-svg')).toBeTruthy());
  expect(axios.get.mock.calls[0][0]).toMatch(/\/vector-clearing\/pnl-history$/);
  expect(axios.get.mock.calls[0][1].params).toEqual({ scope: 'all', hours: 24 });
  const last = screen.getByTestId('pnl-history-last').textContent;
  expect(last).toMatch(/R \+16\.0/);          // группа «Итого»: R
  expect(last).toMatch(/U -1\.00/);           // U итого
  expect(last).toMatch(/R \+ U \+15\.0/);     // R + U итого
  expect(last).toMatch(/агентов 61, без части данных: 4/);
  ['r', 'u', 'total'].forEach((k) => expect(screen.getByTestId(`pnl-history-line-${k}`).getAttribute('d')).toMatch(/^M/));
});

test('группа переключается без нового запроса: значения выбираются из записанных групп, не считаются', async () => {
  axios.get.mockResolvedValue({ data: totalsDto(POINTS) });
  render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-svg')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pnl-history-group-internal'));
  expect(screen.getByTestId('pnl-history-last').textContent).toMatch(/R \+2\.50/);
  expect(screen.getByTestId('pnl-history-last').textContent).toMatch(/U 0\.00/);     // внутренняя биржа: U всегда 0
  fireEvent.click(screen.getByTestId('pnl-history-group-external'));
  expect(screen.getByTestId('pnl-history-last').textContent).toMatch(/R \+13\.5/);
  expect(screen.getByTestId('pnl-history-last').textContent).toMatch(/U -1\.00/);
  expect(axios.get).toHaveBeenCalledTimes(1);
});

test('охват и окно меняют запрос; ответ на прежний запрос не затирает новый', async () => {
  axios.get.mockResolvedValue({ data: totalsDto(POINTS) });
  render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-svg')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pnl-history-scope-translator'));
  await waitFor(() => expect(axios.get.mock.calls.some((c) => c[1].params.scope === 'translator' && c[1].params.hours === 24)).toBe(true));
  fireEvent.click(screen.getByTestId('pnl-history-window-6'));
  await waitFor(() => expect(axios.get.mock.calls.some((c) => c[1].params.scope === 'translator' && c[1].params.hours === 6)).toBe(true));
});

test('выбран агент: запрос истории агента, переключатель охвата заменён подписью, «все агенты» снимает выбор', async () => {
  const onClear = jest.fn();
  axios.get.mockResolvedValue({ data: {
    agent_id: 'T_BTC_binance', window_hours: 24, since_ms: 0, first_ms: 1000, has_data: true,
    points: [{ t: 1000, r: 10, u: 2, total: 12, s: null, groups: { internal: g(1, 0), external: g(10, 2), total: g(11, 2) } },
             { t: 61000, r: 12, u: 3, total: 15, s: null, groups: { internal: g(2, 0), external: g(12, 3), total: g(14, 3) } }],
  } });
  render(<PnlHistoryChart selectedAgent="T_BTC_binance" onClearAgent={onClear} pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-svg')).toBeTruthy());
  expect(axios.get.mock.calls[0][0]).toMatch(/\/vector-clearing\/agent-pnl-history$/);
  expect(axios.get.mock.calls[0][1].params).toEqual({ agent_id: 'T_BTC_binance', hours: 24 });
  expect(screen.getByTestId('pnl-history-agent').textContent).toMatch(/T_BTC_binance/);
  expect(screen.queryByTestId('pnl-history-scopes')).toBeNull();
  expect(screen.getByTestId('pnl-history-last').textContent).toMatch(/R \+14\.0/);   // итого по агенту
  expect(screen.getByTestId('pnl-history-last').textContent).not.toMatch(/агентов/);  // счётчик агентов — только у итогов
  fireEvent.click(screen.getByTestId('pnl-history-clear'));
  expect(onClear).toHaveBeenCalled();
});

test('неизвестное значение — разрыв линии, а не ноль: U неизвестно в одной точке', async () => {
  axios.get.mockResolvedValue({ data: totalsDto([
    pt(1000, g(1, 0), g(10, 2), g(11, 2)),
    pt(61000, g(2, 0), g(12, null), g(14, null)),      // U внешних неизвестно: U и R+U итого — null
    pt(121000, g(3, 0), g(13, 1), g(16, 1)),
  ]) });
  render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-svg')).toBeTruthy());
  const d = screen.getByTestId('pnl-history-line-u').getAttribute('d');
  expect((d.match(/M/g) || []).length).toBe(2);        // линия U состоит из двух отрезков, середина пропущена
  expect(screen.getByTestId('pnl-history-line-r').getAttribute('d').match(/M/g).length).toBe(1);   // R известен во всех точках
});

test('нет записей → понятное сообщение; ошибка загрузки показана и не роняет компонент', async () => {
  axios.get.mockResolvedValueOnce({ data: totalsDto([]) });
  const { unmount } = render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-empty')).toBeTruthy());
  expect(screen.getByTestId('pnl-history-empty').textContent).toMatch(/копится/);
  expect(screen.queryByTestId('pnl-history-svg')).toBeNull();
  unmount();
  axios.get.mockRejectedValue(new Error('boom'));
  render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-error')).toBeTruthy());
});

test('ряды выключаются галочками, выбор запоминается; без отмеченных рядов — подсказка', async () => {
  axios.get.mockResolvedValue({ data: totalsDto(POINTS) });
  render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-svg')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pnl-history-toggle-u'));
  expect(screen.queryByTestId('pnl-history-line-u')).toBeNull();
  expect(JSON.parse(window.localStorage.getItem('pnl.history.series')).u).toBe(false);
  fireEvent.click(screen.getByTestId('pnl-history-toggle-r'));
  fireEvent.click(screen.getByTestId('pnl-history-toggle-total'));
  expect(screen.getByTestId('pnl-history-none')).toBeTruthy();
});

test('ответ на прежний запрос приходит позже нового и не затирает его', async () => {
  let resolveFirst;
  const first = new Promise((res) => { resolveFirst = res; });
  axios.get.mockImplementationOnce(() => first)
    .mockResolvedValue({ data: totalsDto([pt(1000, g(1, 0), g(5, 0), g(6, 0))], 'translator') });
  render(<PnlHistoryChart pollMs={600000} />);
  fireEvent.click(screen.getByTestId('pnl-history-scope-translator'));   // второй запрос (другой охват) успевает раньше первого
  await waitFor(() => expect(screen.getByTestId('pnl-history-last').textContent).toMatch(/R \+6\.00/));
  await act(async () => { resolveFirst({ data: totalsDto(POINTS) }); });   // запоздавший ответ прежнего запроса
  expect(screen.getByTestId('pnl-history-last').textContent).toMatch(/R \+6\.00/);
  expect(screen.getByTestId('pnl-history-last').textContent).not.toMatch(/R \+16\.0/);
});

test('пропуск записей во времени (ledger был недоступен) — разрыв линии, а не прямая через простой', async () => {
  const step = 60000;
  axios.get.mockResolvedValue({ data: totalsDto([0, 1, 2, 3, 12, 13, 14].map((i) => pt(1000 + i * step, g(i, 0), g(i, 0), g(i, 0)))) });
  render(<PnlHistoryChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-history-svg')).toBeTruthy());
  expect(screen.getByTestId('pnl-history-line-r').getAttribute('d').match(/M/g).length).toBe(2);
});
