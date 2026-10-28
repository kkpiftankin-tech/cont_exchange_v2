import React from 'react';
import { render, screen, waitFor, fireEvent, act } from '@testing-library/react';
import axios from 'axios';
import PairTradesChart from '../PairTradesChart';

jest.mock('axios', () => ({ __esModule: true, default: { get: jest.fn(), post: jest.fn() } }));

const T0 = 1793200000000;
const STEP = 900000;
const pt = (i, o = {}) => ({
  t: T0 + i * STEP, buy_qty: 0.4, sell_qty: 0, buy_price: 80030, buy_price_fee: 80094, sell_price: null, sell_price_fee: null,
  cum_buy_qty: 0.4 * (i + 1), cum_sell_qty: 0.1 * i, cum_total_qty: 0.5 * i + 0.4, cum_net_qty: 0.3 * i + 0.4, ...o,
});
const pair = (name, base, quote, fills, n = 3, mk = pt) => ({
  pair: name, base, quote, fills,
  totals: { buy_qty: 1.2, sell_qty: 0.3, total_qty: 1.5, net_qty: 0.9, buy_price: 80030, buy_price_fee: 80094, sell_price: 80100, sell_price_fee: 80036, sell_fee: 64 },
  points: Array.from({ length: n }, (_, i) => mk(i)),
});
const dto = (pairs, over = {}) => ({ window_hours: 24, bucket_ms: STEP, first_ms: T0, fee: { maker_bps: 2, taker_bps: 10, source: 'config' }, pairs, has_data: pairs.length > 0, ...over });
const PAIRS = [pair('BTC/USDT', 'BTC', 'USDT', 120), pair('ETH/BTC', 'ETH', 'BTC', 40, 3, (i) => pt(i, { buy_price: 0.051, sell_price: 0.0515, sell_qty: 1 })), pair('SOL/USDT', 'SOL', 'USDT', 10)];

afterEach(() => { jest.clearAllMocks(); try { window.localStorage.clear(); } catch (e) { /* нет хранилища */ } });

test('по умолчанию отмечены все пары с данными и для каждой свой график; запрос окна 24 ч; две оси и линии объёма и цены', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(axios.get.mock.calls[0][0]).toMatch(/\/vector-clearing\/pair-trades-history$/);
  expect(axios.get.mock.calls[0][1].params).toEqual({ hours: 24 });
  ['BTC/USDT', 'ETH/BTC', 'SOL/USDT'].forEach((n) => {
    expect(screen.getByTestId(`pair-check-${n}`).checked).toBe(true);
    expect(screen.getByTestId(`pair-chart-${n}`)).toBeTruthy();
  });
  expect(screen.getByTestId('pair-line-cum_total_qty-BTC/USDT').getAttribute('d')).toMatch(/^M/);
  expect(screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d')).toMatch(/^M/);
  expect(screen.queryByTestId('pair-line-sell_price-BTC/USDT')).toBeNull();   // продаж в точках нет — линии цены продажи нет
  const svg = screen.getByTestId('pair-svg-BTC/USDT').textContent;
  expect(svg).toMatch(/объём, BTC/);
  expect(svg).toMatch(/цена, USDT/);
  expect(screen.getByTestId('pair-totals-BTC/USDT').textContent).toMatch(/куплено 1\.20 BTC по 80\s?030/);
});

test('чекбоксы пар выключают и включают графики; выбор запоминается; кросс-пара подписана во второй валюте', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-svg-ETH/BTC').textContent).toMatch(/цена, BTC/);      // кросс-пара подписана во второй валюте
  expect(screen.getByTestId('pair-line-sell_price-ETH/BTC')).toBeTruthy();
  fireEvent.click(screen.getByTestId('pair-check-BTC/USDT'));
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
  expect(screen.getByTestId('pair-chart-ETH/BTC')).toBeTruthy();
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual(['ETH/BTC', 'SOL/USDT']);
  fireEvent.click(screen.getByTestId('pair-check-ETH/BTC'));
  fireEvent.click(screen.getByTestId('pair-check-SOL/USDT'));
  expect(screen.getByTestId('pair-none')).toBeTruthy();
  fireEvent.click(screen.getByTestId('pair-check-SOL/USDT'));
  expect(screen.getByTestId('pair-chart-SOL/USDT')).toBeTruthy();
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

test('сохранённый выбор без известных пар не оставляет пустой экран: отмечены все пары с данными; неизвестные имена отбрасываются при следующем выборе', async () => {
  window.localStorage.setItem('pnl.pairs.selected', JSON.stringify(['DOGE/USDT']));
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  ['BTC/USDT', 'ETH/BTC', 'SOL/USDT'].forEach((n) => expect(screen.getByTestId(`pair-check-${n}`).checked).toBe(true));
  expect(screen.queryByTestId('pair-none')).toBeNull();
  fireEvent.click(screen.getByTestId('pair-check-ETH/BTC'));
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual(['BTC/USDT', 'SOL/USDT']);
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

test('чекбокс «− комиссия с цены продажи» независим от чекбокса покупки: линия продажи берёт готовое поле sell_price_fee, покупка остаётся прежней', async () => {
  const mk = (i) => pt(i, { buy_price: 80000 + i * 10, buy_price_fee: 80064 + i * 10, sell_price: 80100 + i * 10, sell_price_fee: 80030 + i * 10, sell_qty: 0.1 });
  axios.get.mockResolvedValue({ data: dto([pair('BTC/USDT', 'BTC', 'USDT', 5, 3, mk)]) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-fee-sell').checked).toBe(false);
  const sellBefore = screen.getByTestId('pair-line-sell_price-BTC/USDT').getAttribute('d');
  expect(screen.getByTestId('pair-totals-BTC/USDT').textContent).not.toMatch(/за вычетом комиссии/);
  fireEvent.click(screen.getByTestId('pair-fee-sell'));
  expect(screen.getByTestId('pair-fee-sell').checked).toBe(true);
  expect(screen.getByTestId('pair-fee').checked).toBe(false);                     // чекбокс покупки не затронут
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.feeSell'))).toBe(true);
  expect(window.localStorage.getItem('pnl.pairs.fee')).toBeNull();
  expect(screen.getByTestId('pair-price-sell_price').parentElement.textContent).toMatch(/цена продажи − комиссия/);
  expect(screen.getByTestId('pair-price-buy_price').parentElement.textContent).toMatch(/цена покупки$/);
  expect(screen.getByTestId('pair-line-sell_price-BTC/USDT').getAttribute('d')).not.toBe(sellBefore);   // продажа опустилась к покупке
  expect(screen.getByTestId('pair-totals-BTC/USDT').textContent).toMatch(/по 80\s?100 \(за вычетом комиссии 80\s?036\)/);
  const svg = screen.getByTestId('pair-svg-BTC/USDT');
  svg.getBoundingClientRect = () => ({ left: 0, width: 780, top: 0, height: 250 });
  fireEvent.mouseMove(svg, { clientX: 780 - 64 });
  expect(screen.getByTestId('pair-hover-BTC/USDT').textContent).toMatch(/продажа 80\s?120 \(за вычетом комиссии 80\s?050\)/);
  expect(screen.getByTestId('pair-hover-BTC/USDT').textContent).toMatch(/покупка 80\s?020 \(/);
  fireEvent.click(screen.getByTestId('pair-fee'));                                // обе комиссии вместе
  expect(screen.getByTestId('pair-hover-BTC/USDT').textContent).toMatch(/покупка 80\s?020 \(с комиссией 80\s?084\)/);
  expect(screen.getByTestId('pair-hover-BTC/USDT').textContent).toMatch(/продажа 80\s?120 \(за вычетом комиссии 80\s?050\)/);
  expect(screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d')).toMatch(/^M/);
  expect(axios.get).toHaveBeenCalledTimes(1);
});

test('нет цены продажи с вычетом комиссии — линии продажи нет, а не ноль; состояние чекбокса продажи восстанавливается из браузера', async () => {
  window.localStorage.setItem('pnl.pairs.feeSell', JSON.stringify(true));
  const mk = (i) => pt(i, { buy_price: 80000, buy_price_fee: 80064, sell_price: 80100, sell_price_fee: null, sell_qty: 0.1 });
  axios.get.mockResolvedValue({ data: dto([pair('BTC/USDT', 'BTC', 'USDT', 5, 3, mk)]) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-fee-sell').checked).toBe(true);
  expect(screen.queryByTestId('pair-line-sell_price-BTC/USDT')).toBeNull();
  expect(screen.getByTestId('pair-line-buy_price-BTC/USDT').getAttribute('d')).toMatch(/^M/);
});

test('все известные пары показаны всегда: пара без сделок в окне неактивна с подписью и датой последней сделки, графика у неё нет', async () => {
  const known = [
    { pair: 'BTC/USDT', base: 'BTC', quote: 'USDT', in_window: true, fills: 120, last_trade_ms: T0 + 2 * STEP },
    { pair: 'SOL/BTC', base: 'SOL', quote: 'BTC', in_window: false, fills: 0, last_trade_ms: T0 - 3600000 },
  ];
  axios.get.mockResolvedValue({ data: dto([PAIRS[0]], { known_pairs: known }) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(screen.getByTestId('pair-check-SOL/BTC')).toBeTruthy();
  expect(screen.getByTestId('pair-check-SOL/BTC').disabled).toBe(true);
  expect(screen.getByTestId('pair-check-SOL/BTC').checked).toBe(false);
  expect(screen.getByTestId('pair-idle-SOL/BTC').textContent).toMatch(/нет сделок за окно, последняя \d{2}\.\d{2}\.\d{4}/);
  expect(screen.queryByTestId('pair-chart-SOL/BTC')).toBeNull();
  expect(screen.getByTestId('pair-check-BTC/USDT').disabled).toBe(false);
  fireEvent.click(screen.getByTestId('pair-select-all'));                  // «все» отмечает известные пары, график появляется только у пар с данными
  expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy();
  expect(screen.queryByTestId('pair-chart-SOL/BTC')).toBeNull();
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual(['BTC/USDT', 'SOL/BTC']);
});

test('выбор по умолчанию + снять одну пару + сменить окно: пара, неактивная в первом окне, получает график во втором (отметка не теряется)', async () => {
  const known = [
    { pair: 'BTC/USDT', base: 'BTC', quote: 'USDT', in_window: true, fills: 120, last_trade_ms: T0 },
    { pair: 'SOL/BTC', base: 'SOL', quote: 'BTC', in_window: false, fills: 0, last_trade_ms: T0 - 7200000 },
  ];
  const wide = [pair('BTC/USDT', 'BTC', 'USDT', 120), pair('SOL/BTC', 'SOL', 'BTC', 7, 3, (i) => pt(i, { buy_price: 0.0021 }))];
  axios.get
    .mockResolvedValueOnce({ data: dto([wide[0]], { known_pairs: known }) })
    .mockResolvedValue({ data: dto(wide, { window_hours: 168, known_pairs: known.map((k) => ({ ...k, in_window: true, fills: 5 })) }) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pair-check-BTC/USDT'));           // снимаем единственную пару с данными
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual(['SOL/BTC']);
  fireEvent.click(screen.getByTestId('pair-window-168'));
  await waitFor(() => expect(screen.getByTestId('pair-chart-SOL/BTC')).toBeTruthy());
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
});

test('кнопки «все» и «снять»: снять убирает все графики и показывает подсказку, все возвращает', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  fireEvent.click(screen.getByTestId('pair-select-none'));
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
  expect(screen.getByTestId('pair-none')).toBeTruthy();
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual([]);
  fireEvent.click(screen.getByTestId('pair-select-all'));
  ['BTC/USDT', 'ETH/BTC', 'SOL/USDT'].forEach((n) => expect(screen.getByTestId(`pair-chart-${n}`)).toBeTruthy());
});

test('в окне нет ни одной сделки: список пар остаётся (все неактивны), сообщение объясняет, где последние сделки', async () => {
  const known = [{ pair: 'ETH/BTC', base: 'ETH', quote: 'BTC', in_window: false, fills: 0, last_trade_ms: T0 - 7200000 }];
  axios.get.mockResolvedValue({ data: dto([], { known_pairs: known }) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-empty')).toBeTruthy());
  expect(screen.getByTestId('pair-empty').textContent).toMatch(/ни по одной паре/);
  expect(screen.getByTestId('pair-check-ETH/BTC').disabled).toBe(true);
  expect(screen.queryByTestId('pair-none')).toBeNull();
});

// ---- фильтр по агенту (CR-F18-010): агент выбирается кликом по строке таблицы «Заработок агентов» на странице и приходит свойством ----
test('агент не выбран: запрос без agent, подсказка как выбрать агента, значка агента нет', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });
  render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(axios.get.mock.calls[0][1].params).toEqual({ hours: 24 });
  expect(screen.getByTestId('pair-agent-hint').textContent).toMatch(/Заработок агентов/);
  expect(screen.queryByTestId('pair-agent-name')).toBeNull();
});

test('агент выбран: запрос с agent, значок с именем и кнопка «все агенты» вызывает сброс', async () => {
  axios.get.mockResolvedValue({ data: dto([PAIRS[0]], { agent: 'T_BTC_binance' }) });
  const onClear = jest.fn();
  render(<PairTradesChart selectedAgent="T_BTC_binance" onClearAgent={onClear} pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  expect(axios.get.mock.calls[0][1].params).toEqual({ hours: 24, agent: 'T_BTC_binance' });
  expect(screen.getByTestId('pair-agent-name').textContent).toBe('T_BTC_binance');
  expect(screen.queryByTestId('pair-agent-hint')).toBeNull();
  expect(screen.queryByTestId('pair-stale')).toBeNull();               // ответ уже по этому агенту
  fireEvent.click(screen.getByTestId('pair-agent-clear'));
  expect(onClear).toHaveBeenCalledTimes(1);
});

test('смена агента: графики и пары прежнего агента убираются сразу (не показываются под значком нового), затем показан ответ по новому агенту', async () => {
  let resolveSecond;
  const second = new Promise((res) => { resolveSecond = res; });
  axios.get.mockResolvedValueOnce({ data: dto(PAIRS) }).mockImplementationOnce(() => second);
  const { rerender } = render(<PairTradesChart pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-ETH/BTC')).toBeTruthy());
  rerender(<PairTradesChart selectedAgent="T_BTC_binance" onClearAgent={() => {}} pollMs={600000} />);
  expect(screen.queryByTestId('pair-chart-ETH/BTC')).toBeNull();
  expect(screen.queryByTestId('pair-check-ETH/BTC')).toBeNull();
  expect(screen.getByText(/Загрузка сделок по парам/)).toBeTruthy();
  expect(axios.get.mock.calls[1][1].params).toEqual({ hours: 24, agent: 'T_BTC_binance' });
  await act(async () => { resolveSecond({ data: dto([PAIRS[0]], { agent: 'T_BTC_binance', known_pairs: [{ pair: 'BTC/USDT', base: 'BTC', quote: 'USDT', in_window: true, fills: 120, last_trade_ms: T0 }] }) }); });
  expect(screen.queryByTestId('pair-stale')).toBeNull();
  expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy();
  expect(screen.queryByTestId('pair-chart-ETH/BTC')).toBeNull();       // у агента нет этой пары — её нет и в списке
  expect(screen.queryByTestId('pair-check-ETH/BTC')).toBeNull();
});

test('прежний BFF не отдаёт поле agent: при выбранном агенте график не показывается, пояснение вместо чужих данных', async () => {
  axios.get.mockResolvedValue({ data: dto(PAIRS) });                   // нет поля agent — сервер агента не понял
  render(<PairTradesChart selectedAgent="T_BTC_binance" onClearAgent={() => {}} pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-error')).toBeTruthy());
  expect(screen.getByTestId('pair-error').textContent).toMatch(/обновите frontend-api/);
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
});

test('сохранённый выбор пар агента читается при открытии; общий выбор на него не влияет', async () => {
  window.localStorage.setItem('pnl.pairs.selected.T_BTC_binance', JSON.stringify(['ETH/BTC']));
  window.localStorage.setItem('pnl.pairs.selected', JSON.stringify(['BTC/USDT']));
  const known = PAIRS.slice(0, 2).map((x) => ({ pair: x.pair, base: x.base, quote: x.quote, in_window: true, fills: x.fills, last_trade_ms: T0 }));
  axios.get.mockResolvedValue({ data: dto(PAIRS.slice(0, 2), { agent: 'T_BTC_binance', known_pairs: known }) });
  render(<PairTradesChart selectedAgent="T_BTC_binance" onClearAgent={() => {}} pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-ETH/BTC')).toBeTruthy());
  expect(screen.queryByTestId('pair-chart-BTC/USDT')).toBeNull();
  expect(screen.getByTestId('pair-check-ETH/BTC').checked).toBe(true);
  expect(screen.getByTestId('pair-check-BTC/USDT').checked).toBe(false);
});

test('выбор пар у агента хранится отдельно от общего выбора: ключ с именем агента, общий не затронут', async () => {
  window.localStorage.setItem('pnl.pairs.selected', JSON.stringify(['SOL/USDT']));
  const known = PAIRS.slice(0, 2).map((p) => ({ pair: p.pair, base: p.base, quote: p.quote, in_window: true, fills: p.fills, last_trade_ms: T0 }));
  axios.get.mockResolvedValue({ data: dto(PAIRS.slice(0, 2), { agent: 'T_BTC_binance', known_pairs: known }) });
  render(<PairTradesChart selectedAgent="T_BTC_binance" onClearAgent={() => {}} pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-chart-BTC/USDT')).toBeTruthy());
  // сохранённый общий выбор (SOL/USDT) на агента не влияет: по умолчанию отмечены все пары агента с данными
  expect(screen.getByTestId('pair-check-BTC/USDT').checked).toBe(true);
  expect(screen.getByTestId('pair-check-ETH/BTC').checked).toBe(true);
  fireEvent.click(screen.getByTestId('pair-check-ETH/BTC'));
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected.T_BTC_binance'))).toEqual(['BTC/USDT']);
  expect(JSON.parse(window.localStorage.getItem('pnl.pairs.selected'))).toEqual(['SOL/USDT']);
  expect(screen.queryByTestId('pair-chart-ETH/BTC')).toBeNull();
});

test('у агента нет хеджей: пояснение с именем агента и без пар в списке', async () => {
  axios.get.mockResolvedValue({ data: dto([], { agent: 'A_USDT_okxuniswap_v3', known_pairs: [] }) });
  render(<PairTradesChart selectedAgent="A_USDT_okxuniswap_v3" onClearAgent={() => {}} pollMs={600000} />);
  await waitFor(() => expect(screen.getByTestId('pair-empty')).toBeTruthy());
  expect(screen.getByTestId('pair-empty').textContent).toMatch(/агента A_USDT_okxuniswap_v3/);
  expect(screen.getByTestId('pair-empty').textContent).toMatch(/за последние 7 суток исполненных хеджей у этого агента не было/);
  expect(screen.queryByTestId('pair-list')).toBeNull();
});
