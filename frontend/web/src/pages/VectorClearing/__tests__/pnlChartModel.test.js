import { niceTicks, fmtUsdt, fmtAxisTime, buildPnlChartModel, nearestPoint } from '../pnlChartModel';

test('niceTicks: шаги 1/2/5·10^k и покрытие диапазона', () => {
  expect(niceTicks(0, 10, 5)).toEqual([0, 2, 4, 6, 8, 10]);
  const t = niceTicks(-3.2, 7.9, 4);
  expect(t.includes(0)).toBe(true);
  expect(t[0]).toBeGreaterThanOrEqual(-3.2);
  expect(t[t.length - 1]).toBeLessThanOrEqual(7.9 + 1e-9);
  expect(niceTicks(5, 5)).toEqual([5]);
});

test('fmtUsdt: знак, масштаб, нет данных', () => {
  expect(fmtUsdt(null)).toBe('—');
  expect(fmtUsdt(undefined, true)).toBe('—');
  expect(fmtUsdt(1.234, true)).toBe('+1.23');
  expect(fmtUsdt(-1.234, true)).toBe('-1.23');
  expect(fmtUsdt(42.56)).toBe('42.6');
  expect(fmtUsdt(0, true)).toBe('0.00');
});

test('fmtAxisTime: в пределах суток — часы:минуты, на длинном окне — с датой', () => {
  const ms = new Date(2026, 9, 22, 14, 5).getTime();
  expect(fmtAxisTime(ms, 3600 * 1000)).toBe('14:05');
  expect(fmtAxisTime(ms, 7 * 24 * 3600 * 1000)).toBe('22.10 14:05');
  expect(fmtAxisTime('x', 1)).toBe('—');
});

test('buildPnlChartModel: пусто → null', () => {
  expect(buildPnlChartModel([], 700, 230)).toBeNull();
  expect(buildPnlChartModel(null, 700, 230)).toBeNull();
});

test('buildPnlChartModel: ось подгоняется под данные, линия идёт слева направо', () => {
  const pts = [
    { t: 1000, r: 1, u: 0, total: 1 },
    { t: 2000, r: 3, u: -1, total: 2 },
    { t: 3000, r: 5, u: 1, total: 6 },
  ];
  const m = buildPnlChartModel(pts, 700, 230);        // по умолчанию отмечены R и R + U
  expect(m.yMin).toBeLessThanOrEqual(1);
  expect(m.yMax).toBeGreaterThanOrEqual(6);
  expect(m.yMin).toBeGreaterThan(0);                  // ноль далеко от данных — ось его не включает
  expect(m.zeroY).toBeNull();
  expect(m.X(1000)).toBeCloseTo(m.m.l, 5);
  expect(m.X(3000)).toBeCloseTo(700 - m.m.r, 5);
  expect(m.Y(m.yMax)).toBeCloseTo(m.m.t, 5);
  expect(m.pathR.startsWith('M')).toBe(true);
  expect(m.pathR.match(/L/g).length).toBe(2);
  expect(m.xTicks.length).toBe(5);
  expect(m.yTicks.length).toBeGreaterThanOrEqual(2);
});

test('buildPnlChartModel: отсутствующее значение разрывает линию, а не соединяет через дыру', () => {
  const pts = [
    { t: 1000, r: 1, u: 0, total: 1 },
    { t: 2000, r: null, u: null, total: 0 },
    { t: 3000, r: 2, u: 0, total: 2 },
  ];
  const m = buildPnlChartModel(pts, 700, 230);
  expect(m.pathR.match(/M/g).length).toBe(2);
  expect(m.pathR.includes('L')).toBe(false);
});

test('buildPnlChartModel: одна точка и плоский ряд не дают деления на ноль', () => {
  const m = buildPnlChartModel([{ t: 5000, r: 0, u: 0, total: 0 }], 700, 230);
  expect(Number.isFinite(m.X(5000))).toBe(true);
  expect(Number.isFinite(m.Y(0))).toBe(true);
  expect(m.yMax).toBeGreaterThan(m.yMin);
});

test('nearestPoint: ближайшая по времени', () => {
  const pts = [{ t: 1000 }, { t: 2000 }, { t: 4000 }];
  expect(nearestPoint(pts, 2400).t).toBe(2000);
  expect(nearestPoint(pts, 3600).t).toBe(4000);
  expect(nearestPoint([], 1)).toBeNull();
});

// Случай владельца: R почти не меняется на фоне большого U — на общей шкале R выглядит прямой.
const flatR = [
  { t: 1000, r: 397.80, u: 4590, total: 4987.8 },
  { t: 2000, r: 397.86, u: 4600, total: 4997.86 },
  { t: 3000, r: 397.83, u: 4580, total: 4977.83 },
  { t: 4000, r: 397.95, u: 4595, total: 4992.95 },
];
const yRange = (m, key) => {
  const ys = flatR.map((p) => m.Y(p[key]));
  return Math.max(...ys) - Math.min(...ys);
};

test('шкала берётся по отмеченным рядам: один R показывает перегибы, на общей шкале с R + U он почти прямой', () => {
  const together = buildPnlChartModel(flatR, 700, 230, { r: true, u: false, total: true });
  const alone = buildPnlChartModel(flatR, 700, 230, { r: true, u: false, total: false });
  expect(yRange(together, 'r')).toBeLessThan(3);      // на общей шкале R — практически прямая
  expect(yRange(alone, 'r')).toBeGreaterThan(100);    // отдельно R занимает большую часть высоты
  expect(alone.yMax - alone.yMin).toBeLessThan(1);
});

test('каждый ряд рисуется только когда отмечен; U — отдельная линия', () => {
  const onlyU = buildPnlChartModel(flatR, 700, 230, { r: false, u: true, total: false });
  expect(onlyU.pathU.startsWith('M')).toBe(true);
  expect(onlyU.pathR).toBe('');
  expect(onlyU.pathTotal).toBe('');
  expect(onlyU.hasVisible).toBe(true);
  const all = buildPnlChartModel(flatR, 700, 230, { r: true, u: true, total: true });
  expect(all.pathR && all.pathU && all.pathTotal).toBeTruthy();
});

test('ни один ряд не отмечен: линий нет, оси определены, hasVisible=false', () => {
  const m = buildPnlChartModel(flatR, 700, 230, { r: false, u: false, total: false });
  expect(m.hasVisible).toBe(false);
  expect(m.pathR + m.pathU + m.pathTotal).toBe('');
  expect(m.yMax).toBeGreaterThan(m.yMin);
  expect(m.yTicks.length).toBeGreaterThanOrEqual(1);
});

test('пунктир нуля есть, только когда ноль внутри диапазона отмеченных рядов', () => {
  const crossing = [{ t: 1000, r: -2, u: 0, total: -2 }, { t: 2000, r: 3, u: 0, total: 3 }];
  expect(buildPnlChartModel(crossing, 700, 230, { r: true, u: false, total: false }).zeroY).not.toBeNull();
  expect(buildPnlChartModel(flatR, 700, 230, { r: true, u: false, total: false }).zeroY).toBeNull();
});

test('плоский отмеченный ряд получает определённую шкалу', () => {
  const flat = [{ t: 1000, r: 5, u: 0, total: 5 }, { t: 2000, r: 5, u: 0, total: 5 }];
  const m = buildPnlChartModel(flat, 700, 230, { r: true, u: false, total: false });
  expect(m.yMax).toBeGreaterThan(m.yMin);
  expect(Number.isFinite(m.Y(5))).toBe(true);
});
