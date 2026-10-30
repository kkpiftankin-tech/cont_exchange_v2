import React from 'react';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import axios from 'axios';
import AgentPnlChart from '../AgentPnlChart';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));

const dto = (points) => ({ agent_id: 'T_BTC_binance', window_hours: 24, since_ms: 0, first_ms: points.length ? points[0].t : null, points, has_data: points.length > 0 });

afterEach(() => { jest.clearAllMocks(); });

test('рисует два ряда R и R+U, текущие значения и подсказки по тексту', async () => {
  axios.get.mockResolvedValue({ data: dto([
    { t: 1000, r: 1, u: 0.5, total: 1.5 },
    { t: 61000, r: 2, u: -0.5, total: 1.5 },
    { t: 121000, r: 3.25, u: 1, total: 4.25 },
  ]) });
  render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
  expect(screen.getByTestId('pnl-line-r').getAttribute('d')).toMatch(/^M/);
  expect(screen.getByTestId('pnl-line-total').getAttribute('d')).toMatch(/^M/);
  const last = screen.getByTestId('pnl-last').textContent;
  expect(last).toMatch(/R \+3\.25/);
  expect(last).toMatch(/U \+1\.00/);
  expect(last).toMatch(/R \+ U \+4\.25/);
  expect(axios.get.mock.calls[0][1].params).toEqual({ agent_id: 'T_BTC_binance', hours: 24 });
});

test('нет истории → понятное сообщение, графика нет', async () => {
  axios.get.mockResolvedValue({ data: dto([]) });
  render(<AgentPnlChart agentId="T_ETH_okx" pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-empty')).toBeTruthy());
  expect(screen.getByTestId('pnl-empty').textContent).toMatch(/копится/);
  expect(screen.queryByTestId('pnl-svg')).toBeNull();
});

test('ошибка загрузки показана и не роняет компонент', async () => {
  axios.get.mockRejectedValue(new Error('boom'));
  render(<AgentPnlChart agentId="T_ETH_okx" pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-error')).toBeTruthy());
});

test('смена окна перезапрашивает историю с новым числом часов', async () => {
  axios.get.mockResolvedValue({ data: dto([{ t: 1000, r: 1, u: 0, total: 1 }, { t: 2000, r: 2, u: 0, total: 2 }]) });
  render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pnl-window-168'));
  await waitFor(() => expect(axios.get.mock.calls.some((c) => c[1].params.hours === 168)).toBe(true));
});

test('смена агента запрашивает историю нового агента', async () => {
  axios.get.mockResolvedValue({ data: dto([]) });
  const { rerender } = render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
  await waitFor(() => expect(axios.get).toHaveBeenCalled());
  rerender(<AgentPnlChart agentId="A_ETH_binanceokx" pollMs={600000} />);
  await waitFor(() => expect(axios.get.mock.calls.some((c) => c[1].params.agent_id === 'A_ETH_binanceokx')).toBe(true));
});

test('устаревший ответ (другое окно) не затирает свежий', async () => {
  let resolveSlow;
  const slow = new Promise((res) => { resolveSlow = res; });
  axios.get
    .mockImplementationOnce(() => Promise.resolve({ data: dto([{ t: 1000, r: 1, u: 0, total: 1 }, { t: 2000, r: 2, u: 0, total: 2 }]) }))   // первая загрузка (24 ч)
    .mockImplementationOnce(() => slow)                                                                                                      // 168 ч — ответ задержан
    .mockImplementation(() => Promise.resolve({ data: dto([{ t: 5000, r: 9, u: 0, total: 9 }, { t: 6000, r: 9.5, u: 0, total: 9.5 }]) })); // возврат на 1 ч
  render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pnl-window-168'));
  fireEvent.click(screen.getByTestId('pnl-window-1'));
  await waitFor(() => expect(screen.getByTestId('pnl-last').textContent).toMatch(/R \+9\.50/));
  resolveSlow({ data: dto([{ t: 1, r: 100, u: 0, total: 100 }, { t: 2, r: 101, u: 0, total: 101 }]) });
  await new Promise((r) => setTimeout(r, 20));
  expect(screen.getByTestId('pnl-last').textContent).toMatch(/R \+9\.50/);
  expect(screen.getByTestId('pnl-last').textContent).not.toMatch(/101/);
});

describe('чекбоксы рядов', () => {
  const series = [
    { t: 1000, r: 397.80, u: 4590, total: 4987.8 },
    { t: 61000, r: 397.86, u: 4600, total: 4997.86 },
    { t: 121000, r: 397.95, u: 4595, total: 4992.95 },
  ];
  beforeEach(() => { try { window.localStorage.clear(); } catch (e) { /* нет хранилища */ } });

  test('по умолчанию отмечены R и R + U, U скрыт; галочка U добавляет линию', async () => {
    axios.get.mockResolvedValue({ data: dto(series) });
    render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
    await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
    expect(screen.getByTestId('pnl-toggle-r').checked).toBe(true);
    expect(screen.getByTestId('pnl-toggle-total').checked).toBe(true);
    expect(screen.getByTestId('pnl-toggle-u').checked).toBe(false);
    expect(screen.queryByTestId('pnl-line-u')).toBeNull();
    fireEvent.click(screen.getByTestId('pnl-toggle-u'));
    expect(screen.getByTestId('pnl-line-u')).toBeTruthy();
  });

  test('снятие галочек скрывает линии и перестраивает шкалу под оставшийся ряд', async () => {
    axios.get.mockResolvedValue({ data: dto(series) });
    render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
    await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
    const dBefore = screen.getByTestId('pnl-line-r').getAttribute('d');
    fireEvent.click(screen.getByTestId('pnl-toggle-total'));
    expect(screen.queryByTestId('pnl-line-total')).toBeNull();
    const dAfter = screen.getByTestId('pnl-line-r').getAttribute('d');
    expect(dAfter).not.toBe(dBefore);                 // шкала сменилась: R растянулся на всю высоту
  });

  test('ни одного ряда — подсказка, график не падает', async () => {
    axios.get.mockResolvedValue({ data: dto(series) });
    render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
    await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
    fireEvent.click(screen.getByTestId('pnl-toggle-r'));
    fireEvent.click(screen.getByTestId('pnl-toggle-total'));
    expect(screen.getByTestId('pnl-none')).toBeTruthy();
    expect(screen.queryByTestId('pnl-line-r')).toBeNull();
  });

  test('выбор рядов запоминается между открытиями графика', async () => {
    axios.get.mockResolvedValue({ data: dto(series) });
    const first = render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
    await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
    fireEvent.click(screen.getByTestId('pnl-toggle-total'));   // выключили R + U
    fireEvent.click(screen.getByTestId('pnl-toggle-u'));       // включили U
    first.unmount();
    render(<AgentPnlChart agentId="A_ETH_binanceokx" pollMs={600000} />);
    await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
    expect(screen.getByTestId('pnl-toggle-total').checked).toBe(false);
    expect(screen.getByTestId('pnl-toggle-u').checked).toBe(true);
  });
});

describe('выгода S (ADR-083)', () => {
  beforeEach(() => { try { window.localStorage.clear(); } catch (e) { /* нет хранилища */ } });
  const arbDto = () => ({
    agent_id: 'A_ETH_binanceokx', window_hours: 24, since_ms: 0, first_ms: 1000, has_data: true, has_surplus: true, surplus_total: 9, surplus_coverage: 0.93,
    points: [{ t: 1000, r: null, u: null, total: null, s: 2 }, { t: 61000, r: null, u: null, total: null, s: 5 }, { t: 121000, r: null, u: null, total: null, s: 9 }],
  });

  test('связка без внешних сделок: график открывается на выгоде S, остальные галочки неактивны, доля учтённых тактов показана', async () => {
    axios.get.mockResolvedValue({ data: arbDto() });
    render(<AgentPnlChart agentId="A_ETH_binanceokx" pollMs={600000} />);
    await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
    expect(screen.getByTestId('pnl-line-s')).toBeTruthy();
    expect(screen.queryByTestId('pnl-line-r')).toBeNull();
    expect(screen.getByTestId('pnl-toggle-s').checked).toBe(true);
    expect(screen.getByTestId('pnl-toggle-r').disabled).toBe(true);
    expect(screen.getByTestId('pnl-last').textContent).toMatch(/S \+9\.00/);
    expect(screen.getByTestId('agent-pnl-chart').textContent).toMatch(/учтено 93 %/);
  });

  test('переводчик с R, U и выгодой: S по умолчанию скрыт, галочка его показывает, ось перестраивается', async () => {
    const dto = { agent_id: 'T_BTC_binance', window_hours: 24, since_ms: 0, first_ms: 1000, has_data: true, has_surplus: true, surplus_total: 3000, surplus_coverage: 1,
      points: [{ t: 1000, r: 1, u: 0, total: 1, s: 1000 }, { t: 61000, r: 2, u: 0, total: 2, s: 2000 }, { t: 121000, r: 3, u: 0, total: 3, s: 3000 }] };
    axios.get.mockResolvedValue({ data: dto });
    render(<AgentPnlChart agentId="T_BTC_binance" pollMs={600000} />);
    await waitFor(() => expect(screen.getByTestId('pnl-svg')).toBeTruthy());
    expect(screen.queryByTestId('pnl-line-s')).toBeNull();
    const before = screen.getByTestId('pnl-line-r').getAttribute('d');
    fireEvent.click(screen.getByTestId('pnl-toggle-s'));
    expect(screen.getByTestId('pnl-line-s')).toBeTruthy();
    expect(screen.getByTestId('pnl-line-r').getAttribute('d')).not.toBe(before);   // общая шкала с S сплющила R
  });
});
