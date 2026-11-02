import { buildPairChartModel, fmtAxis, VOLUME_MODES } from '../pairChartModel';

const STEP = 900000;   // 15 мин
const P = (i, extra = {}) => ({ t: 1000000 + i * STEP, cum_buy_qty: i, cum_sell_qty: i / 2, cum_total_qty: 1.5 * i, cum_net_qty: i / 2, ...extra });

test('fmtAxis: нет значения, нули, крупные и малые числа', () => {
  expect(fmtAxis(null)).toBe('—');
  expect(fmtAxis(0)).toBe('0');
  expect(fmtAxis(80030)).toMatch(/80\s?030/);
  expect(fmtAxis(12.345)).toBe('12.3');
  expect(fmtAxis(2.5)).toBe('2.50');
  expect(fmtAxis(0.0515)).toBe('0.0515');
  expect(fmtAxis(0.00012)).toBe('0.000120');
});

test('пусто → null; одна точка не делит на ноль', () => {
  expect(buildPairChartModel([], 700, 230)).toBeNull();
  const m = buildPairChartModel([P(1, { buy_price: 80000 })], 700, 230, 'total', { buy_price: true, sell_price: true }, STEP);
  expect(m).not.toBeNull();
  expect(Number.isFinite(m.X(m.t0))).toBe(true);
  expect(Number.isFinite(m.YV(1.5))).toBe(true);
});

test('режим объёма выбирает готовые поля: общий и нетто — одна линия, раздельно — две', () => {
  const pts = [1, 2, 3].map((i) => P(i, { buy_price: 80000 + i }));
  expect(buildPairChartModel(pts, 700, 230, 'total').volumeLines.map((l) => l.field)).toEqual(['cum_total_qty']);
  expect(buildPairChartModel(pts, 700, 230, 'net').volumeLines.map((l) => l.field)).toEqual(['cum_net_qty']);
  expect(buildPairChartModel(pts, 700, 230, 'split').volumeLines.map((l) => l.field)).toEqual(['cum_buy_qty', 'cum_sell_qty']);
  expect(VOLUME_MODES.map((m) => m.key)).toEqual(['total', 'net', 'split']);
  expect(buildPairChartModel(pts, 700, 230, 'нет такого').mode).toBe('total');   // неизвестный режим → общий
});

test('две независимые шкалы: объём от нуля, цены по своему диапазону с подписями справа', () => {
  const pts = [1, 2, 3].map((i) => P(i, { buy_price: 80000 + i * 10, sell_price: 80100 + i * 10 }));
  const m = buildPairChartModel(pts, 700, 230, 'total', { buy_price: true, sell_price: true }, STEP);
  expect(m.vScale.lo).toBeLessThanOrEqual(0);
  expect(m.pScale.lo).toBeGreaterThan(79000);                    // цены не прижаты к нулю
  expect(m.yTicksV.length).toBeGreaterThan(1);
  expect(m.yTicksP.length).toBeGreaterThan(1);
  // та же точка во времени, но две разные Y: объём 1,5 и цена 80 010 лежат на своих шкалах
  expect(m.YV(0)).toBeGreaterThan(m.YV(4.5));
  expect(m.YP(80010)).toBeGreaterThan(m.YP(80130));
});

test('цены скрыты → правая шкала не строится; видима одна сторона → одна линия', () => {
  const pts = [1, 2, 3].map((i) => P(i, { buy_price: 80000 + i, sell_price: 80100 + i }));
  const none = buildPairChartModel(pts, 700, 230, 'total', { buy_price: false, sell_price: false }, STEP);
  expect(none.hasPrice).toBe(false);
  expect(none.priceLines).toEqual([]);
  const one = buildPairChartModel(pts, 700, 230, 'total', { buy_price: true, sell_price: false }, STEP);
  expect(one.priceLines.map((l) => l.key)).toEqual(['buy_price']);
});

test('линия цены соединяет точки с ценой и пропускает интервалы без цены; простой больше 4 интервалов рвёт линию', () => {
  const pts = [
    P(0, { buy_price: 80000 }), P(1, { buy_price: null }), P(2, { buy_price: 80020 }),   // интервал без покупок пропущен, соседние точки соединены
    P(10, { buy_price: 80100 }), P(11, { buy_price: 80110 }),                              // простой в 8 интервалов — разрыв
  ];
  const m = buildPairChartModel(pts, 700, 230, 'total', { buy_price: true, sell_price: false }, STEP);
  const d = m.priceLines[0].d;
  expect((d.match(/M/g) || []).length).toBe(2);
  expect((d.match(/L/g) || []).length).toBe(2);   // отрезки: 0→2 и 10→11
});

test('линия объёма ступенчатая и непрерывная: от нуля в начале окна, скачок в каждой точке, уровень до конца окна', () => {
  const pts = [0, 1, 2, 10, 11].map((i) => P(i));
  const m = buildPairChartModel(pts, 700, 230, 'total', { buy_price: false, sell_price: false }, STEP);
  const d = m.volumeLines[0].d;
  expect((d.match(/M/g) || []).length).toBe(1);       // один отрезок, простой между точками линию не рвёт (накопленное не меняется)
  expect((d.match(/L/g) || []).length).toBe(2 * pts.length + 1);   // по две вершины на точку (горизонталь, скачок) и конец окна
  const nums = d.replace(/[ML]/g, ' ').trim().split(/\s+/).map(Number);
  expect(nums[0]).toBeCloseTo(m.X(m.t0), 1);          // начало — левый край окна
  expect(nums[1]).toBeCloseTo(m.YV(0), 1);            // на уровне нуля
  expect(nums[nums.length - 2]).toBeCloseTo(m.X(m.t1), 1);   // конец — правый край
  expect(nums[nums.length - 1]).toBeCloseTo(m.YV(pts[pts.length - 1].cum_total_qty), 1);
});

test('окно запроса задаёт ось времени целиком: сделки только в конце окна занимают его правый край', () => {
  const pts = [P(0, { buy_price: 80000 }), P(1, { buy_price: 80010 })];
  const win = { t0: pts[0].t - 20 * STEP, t1: pts[1].t + 2 * STEP };
  const m = buildPairChartModel(pts, 700, 230, 'total', { buy_price: true, sell_price: false }, STEP, win);
  expect(m.t0).toBe(win.t0);
  expect(m.t1).toBe(win.t1);
  expect(m.X(pts[0].t)).toBeGreaterThan(m.X(m.t0) + 0.5 * m.pw);
  expect(buildPairChartModel(pts, 700, 230, 'total', { buy_price: true, sell_price: false }, STEP, null).t0).toBe(pts[0].t);   // без окна — по точкам
  const inside = buildPairChartModel(pts, 700, 230, 'total', { buy_price: true }, STEP, { t0: pts[0].t + STEP, t1: pts[1].t - STEP });
  expect(inside.t0).toBe(pts[0].t);                    // окно не сужает ось: точки всегда на графике
});

test('одиночная точка цены рисуется как точка (M и L в одну позицию), а не пропадает', () => {
  const pts = [P(0, { buy_price: null }), P(1, { buy_price: 80000 }), P(2, { buy_price: null })];
  const d = buildPairChartModel(pts, 700, 230, 'total', { buy_price: true, sell_price: false }, STEP).priceLines[0].d;
  const nums = d.replace(/[ML]/g, ' ').trim().split(/\s+/).map(Number);
  expect(d).toMatch(/^M[\d.]+ [\d.]+ L[\d.]+ [\d.]+$/);
  expect(nums[0]).toBe(nums[2]);
  expect(nums[1]).toBe(nums[3]);
  const split = [P(0, { sell_price: 80100 }), P(30, { sell_price: 80120 })];      // две изолированные цены, разрыв больше 4 интервалов
  const ds = buildPairChartModel(split, 700, 230, 'total', { buy_price: false, sell_price: true }, STEP).priceLines[0].d;
  expect((ds.match(/M/g) || []).length).toBe(2);
});

test('плоский нулевой ряд не даёт подписей вида 1e-9: шкала раздвигается на ±1', () => {
  const zero = [P(0, { cum_net_qty: 0 }), P(1, { cum_net_qty: 0 })];
  const m = buildPairChartModel(zero, 700, 230, 'net', { buy_price: false, sell_price: false }, STEP);
  expect(m.vScale.lo).toBeLessThan(-0.5);
  expect(m.vScale.hi).toBeGreaterThan(0.5);
  expect(m.yTicksV.map((t) => fmtAxis(t.v)).join(' ')).not.toMatch(/e-/);
});
