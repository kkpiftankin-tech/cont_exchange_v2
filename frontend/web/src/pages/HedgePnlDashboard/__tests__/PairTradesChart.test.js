import React from 'react';
import { render, screen, waitFor, fireEvent, act } from '@testing-library/react';
import axios from 'axios';
import PairTradesChart from '../PairTradesChart';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));

const T0 = 1793200000000;
const STEP = 900000;
const pt = (i, o = {}) => ({
  t: T0 + i * STEP, buy_qty: 0.4, sell_qty: 0, buy_price: 80030, buy_price_fee: 80094, sell_price: null,
  cum_buy_qty: 0.4 * (i + 1), cum_sell_qty: 0.1 * i, cum_total_qty: 0.5 * i + 0.4, cum_net_qty: 0.3 * i + 0.4, ...o,
});
const pair = (name, base, quote, fills, n = 3, mk = pt) => ({
  pair: name, base, quote, fills,
  totals: { buy_qty: 1.2, sell_qty: 0.3, total_qty: 1.5, net_qty: 0.9, buy_price: 80030, buy_price_fee: 80094, sell_price: 80100 },
  points: Array.from({ length: n }, (_, i) => mk(i)),
});
const dto = (pairs, over = {}) => ({ window_hours: 24, bucket_ms: STEP, first_ms: T0, fee: { maker_bps: 2, taker_bps: 10, source: 'config' }, pairs, has_data: pairs.length > 0, ...over });
const PAIRS = [pair('BTC/USDT', 'BTC', 'USDT', 120), pair('ETH/BTC', 'ETH', 'BTC', 40, 3, (i) => pt(i, { buy_price: 0.051, sell_price: 0.0515, sell_qty: 1 })), pair('SOL/USDT', 'SOL', 'USDT', 10)];

afterEach(() => { jest.clearAllMocks(); try { window.localStorage.clear(); } catch (e) { /* нет хранилища */ } });

test('по умолчанию отмечена самая активная пара; запрос окна 24 ч; две оси и линии объёма и цены', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(axios.get.mock.calls[0][0]).toMatch(/\/vector-clearing\/pair-trades-history$/);
  expect(axios.get.mock.calls[0][1].params).toEqual({ hours: 24 });
  expect(screen.getByTestId('pair-check-BTC/USDT').checked).toBe(true);
  expect(screen.getByTestId('pair-check-ETH/BTC').checked).toBe(false);
  expect(screen.queryByTestId('pair-chart-ETH/BTC')).toBeNull();
  expect(screen.getByTestId('pair-line-cum_total_qty-BTC/USDT').getAttribute('d')).toMatch(/^M/);
  expect(screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d')).toMatch(/^M/);
  expect(screen.queryByTestId('pair-line-sell_price-BTC/USDT')).toBeNull();   // продаж в точках нет — линии цены продажи нет
  const svg = screen.getByTestId('pair-svg-BTC/USDT').textContent;
  expect(svg).toMatch(/объём, BTC/);
  expect(svg).toMatch(/цена, USDT/);
  expect(screen.getByTestId('pair-totals-BTC/USDT').textContent).toMatch(/куплено 1\.20 BTC по 80\s?030/);
});

test('чекбоксы пар включают и выключают графики; выбор запоминается; кросс-пара подписана во второй валюте', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pair-check-ETH/BTC'));
  expect(screen.getByTestId('pair-chart-ETH/BTC')).toBeTruthy();
  expect(screen.getByTestId('pair-svg-ETH/BTC').textContent).toMatch(/цена, BTC/);
  expect(screen.getByTestId('pair-line-sell_price-ETH/BTC')).toBeTruthy();
  fireEvent.click(screen.getByTestId('pair-check-BTC/USDT'));
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual(['ETH/BTC']);
  fireEvent.click(screen.getByTestId('pair-check-ETH/BTC'));
  expect(screen.getByTestId('pair-none')).toBeTruthy();
});

test('режим объёма выбирает готовое поле без запроса: нетто — одна линия, раздельно — две', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  fireEvent.change(screen.getByTestId('pair-mode'), { target: { value: 'net' } });
  expect(screen.getByTestId('pair-line-cum_net_qty-BTC/USDT')).toBeTruthy();
  expect(screen.queryByTestId('pair-line-cum_total_qty-BTC/USDT')).toBeNull();
  fireEvent.change(screen.getByTestId('pair-mode'), { target: { value: 'split' } });
  expect(screen.getByTestId('pair-line-cum_buy_qty-BTC/USDT')).toBeTruthy();
  expect(screen.getByTestId('pair-line-cum_sell_qty-BTC/USDT')).toBeTruthy();
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.mode'))).toBe('split');
  expect(axios.get).toHaveBeenCalledTimes(1);
});

test('галочки цен скрывают линии и правую ось', async () => {
  axios.get.mockResolvedValue({ data: dto([PAIRS[1]]) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-ETH/BTC')).toBeTruthy());
  expect(screen.getByTestId('pair-line-buy_price-ETH/BTC')).toBeTruthy();
  fireEvent.click(screen.getByTestId('pair-price-buy_price'));
  expect(screen.queryByTestId('pair-line-buy_price-ETH/BTC')).toBeNull();
  expect(screen.getByTestId('pair-line-sell_price-ETH/BTC')).toBeTruthy();
  fireEvent.click(screen.getByTestId('pair-price-sell_price'));
  expect(screen.queryByTestId('pair-line-sell_price-ETH/BTC')).toBeNull();
  expect(screen.getByTestId('pair-svg-ETH/BTC').textContent).not.toMatch(/цена, BTC/);
});

test('окно меняет запрос; запоздавший ответ прежнего окна не затирает новый', async () => {
  let resolveFirst;
  const first = new Promise((res) => { resolveFirst = res; });
  axios.get.mockImplementationOnce(() => first).mockResolvedValue({ data: dto([pair('SOL/USDT', 'SOL', 'USDT', 5)], { window_hours: 6 }) });
  render(<PairTradesChart pollMs={600000} />);
  fireEvent.click(screen.getByTestId('pair-window-6'));
  await waitFor(() => expect(screen.getByTestId('pair-chart-SOL/USDT')).toBeTruthy());
  expect(axios.get.mock.calls[1][1].params).toEqual({ hours: 6 });
  await act(async () => { resolveFirst({ data: dto(PAIRS) }); });
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
  expect(screen.getByTestId('pair-chart-SOL/USDT')).toBeTruthy();
});

test('подсказка под курсором показывает объём и цены точки, значения не пересчитываются', async () => {
  axios.get.mockResolvedValue({ data: dto([PAIRS[1]]) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-ETH/BTC')).toBeTruthy());
  expect(screen.getByTestId('pair-hover-ETH/BTC').textContent).toMatch(/точек: 3/);
  const svg = screen.getByTestId('pair-svg-ETH/BTC');
  svg.getBoundingClientRect = () => ({ left: 0, width: 780, top: 0, height: 250 });
  fireEvent.mouseMove(svg, { clientX: 780 - 64 });   // правый край рабочей области: последняя точка
  const txt = screen.getByTestId('pair-hover-ETH/BTC').textContent;
  expect(txt).toMatch(/объём 1\.40 ETH/);            // cum_total_qty последней точки = 0,5·2 + 0,4
  expect(txt).toMatch(/покупка 0\.0510/);
  expect(txt).toMatch(/продажа 0\.0515/);
});

test('нет сделок → сообщение; ошибка загрузки показана и не роняет компонент', async () => {
  axios.get.mockResolvedValueOnce({ data: dto([]) });
  const { unmount } = render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-empty')).toBeTruthy());
  expect(screen.queryByTestId('pair-list')).toBeNull();
  unmount();
  axios.get.mockRejectedValue(new Error('boom'));
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-error')).toBeTruthy());
});

test('сохранённый выбор без пар из ответа не оставляет пустой экран: показывается самая активная пара', async () => {
  window.localStorage.setItem('pnl.pairs.selected', JSON.stringify(['DOGE/USDT']));
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-check-BTC/USDT').checked).toBe(true);
  expect(screen.queryByTestId('pair-none')).toBeNull();
  fireEvent.click(screen.getByTestId('pair-check-ETH/BTC'));          // пара, которой нет в ответе, в сохранённом выборе не теряется
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual(['DOGE/USDT', 'BTC/USDT', 'ETH/BTC']);
});

test('пользователь снял все пары: пустой выбор уважается и показывается подсказка', async () => {
  window.localStorage.setItem('pnl.pairs.selected', JSON.stringify([]));
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-none')).toBeTruthy());
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
});

test('ряд из одной точки виден (линия цены — точка), на оси времени всё окно запроса', async () => {
  const one = { ...pair('BTC/USDT', 'BTC', 'USDT', 3, 1), points: [pt(0, { sell_price: 80100, sell_qty: 0.2 })] };
  axios.get.mockResolvedValue({ data: dto([one], { since_ms: T0 - 24 * 3600 * 1000, generatedAt: new Date(T0 + 5 * STEP).toISOString() }) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d')).toMatch(/^M[\d.]+ [\d.]+ L/);
  expect(screen.getByTestId('pair-line-cum_total_qty-BTC/USDT').getAttribute('d').match(/L/g).length).toBeGreaterThan(2);
});

test('разрыв линии цены при простое виден в компоненте', async () => {
  const gap = { ...pair('BTC/USDT', 'BTC', 'USDT', 9, 0), points: [0, 1, 2, 12, 13].map((i) => pt(i, { sell_price: null })) };
  axios.get.mockResolvedValue({ data: dto([gap]) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d').match(/M/g).length).toBe(2);
});

test('смена окна: пока нет ответа, прежний график приглушён и подписан; кнопка окна нажата (aria-pressed)', async () => {
  let resolveSecond;
  const second = new Promise((res) => { resolveSecond = res; });
  axios.get.mockResolvedValueOnce({ data: dto(PAIRS) }).mockImplementationOnce(() => second);
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.queryByTestId('pair-stale')).toBeNull();
  fireEvent.click(screen.getByTestId('pair-window-168'));
  expect(screen.getByTestId('pair-window-168').getAttribute('aria-pressed')).toBe('true');
  expect(screen.getByTestId('pair-stale').textContent).toMatch(/168 ч/);
  await act(async () => { resolveSecond({ data: dto(PAIRS, { window_hours: 168 }) }); });
  expect(screen.queryByTestId('pair-stale')).toBeNull();
});

test('чекбокс «+ комиссия к цене покупки»: линия покупки берёт готовое поле buy_price_fee, итоги и подсказка показывают цену с комиссией, запроса нет', async () => {
  // цена покупки меняется по времени, комиссия — фиксированная надбавка в готовом поле; есть и цена продажи, поэтому сдвиг линии покупки
  // относительно линии продажи заметен (шкала цен общая для обеих линий)
  const mk = (i) => pt(i, { buy_price: 80000 + i * 10, buy_price_fee: 80064 + i * 10, sell_price: 80100, sell_qty: 0.1 });
  axios.get.mockResolvedValue({ data: dto([pair('BTC/USDT', 'BTC', 'USDT', 5, 3, mk)]) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-fee').checked).toBe(false);
  expect(screen.getByTestId('pair-fee-rates').textContent).toMatch(/maker 2 \/ taker 10 б\.п\./);
  expect(screen.getByTestId('pair-totals-BTC/USDT').textContent).not.toMatch(/с комиссией/);
  const before = screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d');
  expect(screen.getByTestId('pair-price-buy_price').parentElement.textContent).toMatch(/цена покупки$/);
  fireEvent.click(screen.getByTestId('pair-fee'));
  expect(screen.getByTestId('pair-fee').checked).toBe(true);
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.fee'))).toBe(true);
  expect(screen.getByTestId('pair-price-buy_price').parentElement.textContent).toMatch(/цена покупки \+ комиссия/);
  expect(screen.getByTestId('pair-totals-BTC/USDT').textContent).toMatch(/по 80\s?030 \(с комиссией 80\s?094\)/);
  const after = screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d');
  expect(after).toMatch(/^M/);
  expect(after).not.toBe(before);                  // линия покупки сдвинулась вверх к линии продажи: координаты взяты по полю с комиссией
  const svg = screen.getByTestId('pair-svg-BTC/USDT');
  svg.getBoundingClientRect = () => ({ left: 0, width: 780, top: 0, height: 250 });
  fireEvent.mouseMove(svg, { clientX: 780 - 64 });
  expect(screen.getByTestId('pair-hover-BTC/USDT').textContent).toMatch(/покупка 80\s?020 \(с комиссией 80\s?084\)/);
  fireEvent.click(screen.getByTestId('pair-fee'));
  expect(screen.getByTestId('pair-totals-BTC/USDT').textContent).not.toMatch(/с комиссией/);
  expect(axios.get).toHaveBeenCalledTimes(1);
});

test('чекбокс комиссии восстанавливается из браузера; линия продажи остаётся; нет цены с комиссией — линии покупки нет, а не ноль', async () => {
  window.localStorage.setItem('pnl.pairs.fee', JSON.stringify(true));
  const mk = (i) => pt(i, { buy_price: 80000, buy_price_fee: null, sell_price: 80100, sell_qty: 0.1 });
  axios.get.mockResolvedValue({ data: dto([pair('BTC/USDT', 'BTC', 'USDT', 5, 3, mk)]) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-fee').checked).toBe(true);
  expect(screen.queryByTestId('pair-line-buy_price-BTC/USDT')).toBeNull();          // покупки с комиссией нет ни в одной точке: линии нет
  expect(screen.getByTestId('pair-line-sell_price-BTC/USDT').getAttribute('d')).toMatch(/^M/);
});
