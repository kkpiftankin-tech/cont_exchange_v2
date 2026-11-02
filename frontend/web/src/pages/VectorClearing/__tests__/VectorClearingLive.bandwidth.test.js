import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import VectorClearingLive from '../VectorClearingLive';

// ADR-081 — режимы ширины полосы хеджа на экране «Клиринг»: от комиссии / авто по волатильности / вручную.
jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));
const axios = require('axios').default;
jest.mock('react-router-dom', () => ({ useNavigate: () => () => {} }), { virtual: true });
jest.mock('../../../api/authService', () => ({ isAuthenticated: () => Promise.resolve(true) }));
jest.mock('../../../components/NavBar', () => () => null);

const config = {
  batch_window_ms: 1000, stale_level_ms: 60000, ce_taker_fee_bps: -1, ce_inv_skew_gamma: 0.006, ce_inv_skew_max_pm: 8,
  slope_method: 3, ce_band_fee_k: 1.8, ce_maker_fee_bps: 2,
  ce_band_manual: false, ce_band_manual_q_lim: 5, ce_band_manual_q_mkt: 25, ce_band_gamma_mode: false,
  ce_band_q_floor: 2, ce_band_sigma_ref: 0.00015, ce_band_vol_ratio_min: 0.1, ce_band_vol_ratio_max: 10, ce_band_mode: 'fee', ce_band_arb_scale: 2,
};

beforeEach(() => {
  axios.get.mockReset(); axios.post.mockReset();
  axios.get.mockImplementation((url) => {
    if (/vector-clearing\/config$/.test(url)) return Promise.resolve({ data: config });
    return Promise.resolve({ data: [] });
  });
  axios.post.mockImplementation((url, body) => Promise.resolve({ data: { ...config, ...body, applied: true } }));
});

const selectMode = async (container) => {
  await waitFor(() => expect(container.querySelector('[data-testid="band-mode"] select')).not.toBeNull());
  return container.querySelector('[data-testid="band-mode"] select');
};
const postedBody = () => axios.post.mock.calls.find((c) => /vector-clearing\/config$/.test(c[0]))[1];

test('по умолчанию режим «от комиссии»: ручных полей нет, сохранение выключает ручной режим и режим по σ', async () => {
  const { container } = render(<VectorClearingLive />);
  const sel = await selectMode(container);
  await waitFor(() => expect(sel.value).toBe('fee'));
  expect(screen.queryByText(/Z̄lim \(тыс\. USDT\)/)).toBeNull();
  fireEvent.click(screen.getByText('Применить'));
  await waitFor(() => expect(axios.post).toHaveBeenCalled());
  const b = postedBody();
  expect(b.ce_band_manual).toBe(false);
  expect(b.ce_band_gamma_mode).toBe(false);
  expect('ce_band_manual_q_lim' in b).toBe(false);
});

test('множитель порога арбитражёров: подхватывается из базы, правится и отправляется; в ручном режиме скрыт и не отправляется', async () => {
  const { container } = render(<VectorClearingLive />);
  const sel = await selectMode(container);
  const input = () => container.querySelector('[data-testid="arb-scale"] input');
  await waitFor(() => expect(input()).not.toBeNull());
  await waitFor(() => expect(input().value).toBe('2'));            // значение из базы
  fireEvent.change(input(), { target: { value: '3' } });
  fireEvent.click(screen.getByText('Применить'));
  await waitFor(() => expect(axios.post).toHaveBeenCalled());
  expect(postedBody().ce_band_arb_scale).toBe(3);
  axios.post.mockClear();
  fireEvent.change(sel, { target: { value: 'manual' } });          // ручной режим: множитель не применяется
  expect(container.querySelector('[data-testid="arb-scale"]')).toBeNull();
  fireEvent.click(screen.getByText('Применить'));
  await waitFor(() => expect(axios.post).toHaveBeenCalled());
  expect('ce_band_arb_scale' in postedBody()).toBe(false);
});

test('ручной режим: поля Z̄lim и Z̄mkt отправляются, режим по σ не трогается', async () => {
  const { container } = render(<VectorClearingLive />);
  const sel = await selectMode(container);
  fireEvent.change(sel, { target: { value: 'manual' } });
  const lim = screen.getByText(/Z̄lim \(тыс\. USDT\)/).querySelector('input');
  const mkt = screen.getByText(/Z̄mkt \(тыс\. USDT\)/).querySelector('input');
  await waitFor(() => expect(lim.value).toBe('5'));
  fireEvent.change(lim, { target: { value: '12' } });
  fireEvent.change(mkt, { target: { value: '60' } });
  expect(container.textContent).toMatch(/ручная ширина: Z̄lim 12 · Z̄mkt 60/);
  fireEvent.click(screen.getByText('Применить'));
  await waitFor(() => expect(axios.post).toHaveBeenCalled());
  const b = postedBody();
  expect(b.ce_band_manual).toBe(true);
  expect(b.ce_band_manual_q_lim).toBe(12);
  expect(b.ce_band_manual_q_mkt).toBe(60);
  expect('ce_band_gamma_mode' in b).toBe(false);
});

test('авто по волатильности: отправляются режим σ, пол, опорная σ и границы множителя', async () => {
  const { container } = render(<VectorClearingLive />);
  const sel = await selectMode(container);
  fireEvent.change(sel, { target: { value: 'volatility' } });
  await waitFor(() => expect(screen.getByText(/опорная σ/)).toBeTruthy());
  fireEvent.change(screen.getByText(/опорная σ/).querySelector('input'), { target: { value: '0.0002' } });
  fireEvent.click(screen.getByText('Применить'));
  await waitFor(() => expect(axios.post).toHaveBeenCalled());
  const b = postedBody();
  expect(b.ce_band_manual).toBe(false);
  expect(b.ce_band_gamma_mode).toBe(true);
  expect(b.ce_band_sigma_ref).toBe(0.0002);
  expect(b.ce_band_q_floor).toBe(2);
  expect(b.ce_band_vol_ratio_min).toBe(0.1);
  expect(b.ce_band_vol_ratio_max).toBe(10);
});

test('пока настройки не прочитаны, «Применить» заблокировано (иначе режим молча сбросился бы)', async () => {
  axios.get.mockImplementation((url) => (/vector-clearing\/config$/.test(url) ? Promise.reject(new Error('down')) : Promise.resolve({ data: [] })));
  const { container } = render(<VectorClearingLive />);
  await selectMode(container);
  expect(screen.getByText('Применить').disabled).toBe(true);
  fireEvent.click(screen.getByText('Применить'));
  expect(axios.post).not.toHaveBeenCalled();
});

test('режим из базы подхватывается при загрузке, ошибка сервера показывается оператору', async () => {
  axios.get.mockImplementation((url) => (/vector-clearing\/config$/.test(url)
    ? Promise.resolve({ data: { ...config, ce_band_manual: true, ce_band_mode: 'manual', ce_band_manual_q_lim: 9, ce_band_manual_q_mkt: 40 } })
    : Promise.resolve({ data: [] })));
  axios.post.mockRejectedValue({ message: 'Request failed', response: { data: { error: 'band_invalid', message: 'ручная ширина: нужно 0 < Z̄lim ≤ Z̄mkt' } } });
  const { container } = render(<VectorClearingLive />);
  const sel = await selectMode(container);
  await waitFor(() => expect(sel.value).toBe('manual'));
  await waitFor(() => expect(screen.getByText(/Z̄lim \(тыс\. USDT\)/).querySelector('input').value).toBe('9'));
  fireEvent.click(screen.getByText('Применить'));
  await waitFor(() => expect(container.textContent).toMatch(/ошибка: ручная ширина: нужно 0 < Z̄lim ≤ Z̄mkt/));
});
