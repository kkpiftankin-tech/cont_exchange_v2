// Модель двухосевого графика «сделки по паре» (презентация): только шкалы, подписи осей и пути линий. Накопленные объёмы и цены
// приходят готовыми из BFF (/api/vector-clearing/pair-trades-history, CALC-CE-PAIR-TRADES) — здесь ничего не пересчитывается
// (frontend-no-domain-compute): режим объёма лишь выбирает, какое готовое поле точки рисовать.
import { niceTicks, fmtAxisTime } from '../VectorClearing/pnlChartModel';

const finite = (v) => v !== null && v !== undefined && Number.isFinite(Number(v));

// Режимы левой оси: какие готовые накопленные поля точки рисуются. Ключ — подпись в переключателе.
export const VOLUME_MODES = [
  { key: 'total', label: 'общий (покупки + продажи)', hint: 'сколько базовой валюты всего прошло через внешние биржи', fields: [['cum_total_qty', 'объём', '#4fb0d8']] },
  { key: 'net', label: 'нетто (покупки − продажи)', hint: 'на сколько базовой валюты за окно куплено больше, чем продано (минус — продано больше)', fields: [['cum_net_qty', 'нетто', '#4fb0d8']] },
  { key: 'split', label: 'покупки и продажи отдельно', hint: 'накопленные покупки и накопленные продажи двумя линиями', fields: [['cum_buy_qty', 'куплено', '#5fd08a'], ['cum_sell_qty', 'продано', '#e06060']] },
];
export const PRICE_SERIES = [
  { key: 'buy_price', label: 'цена покупки', color: '#5fd08a' },
  { key: 'sell_price', label: 'цена продажи', color: '#e06060' },
];

// Подпись числа на оси: крупные без дробной части, малые с нужной точностью по масштабу значений.
export function fmtAxis(v) {
  if (!finite(v)) return '—';
  const n = Number(v);
  const a = Math.abs(n);
  if (a === 0) return '0';
  if (a >= 1000) return n.toLocaleString('ru-RU', { maximumFractionDigits: 0 });
  if (a >= 10) return n.toFixed(1);
  if (a >= 1) return n.toFixed(2);
  if (a >= 0.01) return n.toFixed(4);
  return n.toPrecision(3);
}

// Шкала по значениям: min..max с отступом; одна точка или плоский ряд раздвигаются (ряд из одних нулей — на ±1, без подписей вида 1e-9).
// zeroBase — шкала включает ноль (накопленный объём считается от нуля).
function scaleOf(values, { zeroBase = false } = {}) {
  let lo = Infinity, hi = -Infinity;
  for (const v of values) if (finite(v)) { lo = Math.min(lo, Number(v)); hi = Math.max(hi, Number(v)); }
  if (!(lo <= hi)) return null;
  if (zeroBase) { lo = Math.min(lo, 0); hi = Math.max(hi, 0); }
  if (!(hi - lo > 1e-12)) {
    const c = Math.abs(hi) < 1e-12 ? 1 : Math.max(Math.abs(hi) * 0.02, 1e-9);
    lo -= c; hi += c;
  }
  const pad = (hi - lo) * 0.08;
  return { lo: zeroBase && lo >= 0 ? lo : lo - pad, hi: hi + pad };
}

// points — точки одной пары из ответа BFF: { t, cum_*_qty, buy_price, sell_price }; bucketMs — размер интервала; mode — ключ VOLUME_MODES;
// visiblePrices — { buy_price, sell_price }; win — { t0, t1 } окно запроса (начало окна и момент ответа): ось времени охватывает всё окно, а не только
// участок со сделками (null — по первой и последней точке). Возвращает null, если рисовать нечего.
// Линия объёма ступенчатая: накопленное значение от начала окна (ноль) растёт скачком в момент сделок и между ними не меняется; продолжается
// до конца окна. Линия цены соединяет соседние точки, где цена есть (интервал без сделок стороны — не точка линии), и рвётся, если между ними
// больше 4 интервалов (простой не интерполируется прямой); одиночная точка цены рисуется как точка.
export function buildPairChartModel(points, W, H, mode = 'total', visiblePrices = { buy_price: true, sell_price: true }, bucketMs = 900000,
  win = null, m = { l: 64, r: 64, t: 12, b: 28 }) {
  const pts = (Array.isArray(points) ? points : []).filter((p) => finite(p && p.t));
  if (pts.length === 0) return null;
  const pw = W - m.l - m.r, ph = H - m.t - m.b;
  const firstT = Number(pts[0].t), lastT = Number(pts[pts.length - 1].t);
  let t0 = win && finite(win.t0) ? Math.min(Number(win.t0), firstT) : firstT;
  let t1 = win && finite(win.t1) ? Math.max(Number(win.t1), lastT) : lastT;
  if (!(t1 > t0)) { t0 -= bucketMs / 2; t1 += bucketMs / 2; }
  const vm = VOLUME_MODES.find((x) => x.key === mode) || VOLUME_MODES[0];
  const vScale = scaleOf(pts.flatMap((p) => vm.fields.map(([f]) => p[f])), { zeroBase: true }) || { lo: 0, hi: 1 };
  const priceKeys = PRICE_SERIES.map((s) => s.key).filter((k) => visiblePrices && visiblePrices[k]);
  const pScale = scaleOf(pts.flatMap((p) => priceKeys.map((k) => p[k])));
  const X = (t) => m.l + ((Number(t) - t0) / (t1 - t0)) * pw;
  const YV = (v) => m.t + (1 - (Number(v) - vScale.lo) / (vScale.hi - vScale.lo)) * ph;
  const YP = (v) => (pScale ? m.t + (1 - (Number(v) - pScale.lo) / (pScale.hi - pScale.lo)) * ph : m.t + ph / 2);
  const gapMs = 4 * bucketMs;
  const f1 = (n) => n.toFixed(1);

  // Линия цены: отрезки между соседними точками с ценой; простой больше gapMs начинает новый отрезок; отрезок из одной точки — точка (M…L в ту же позицию).
  const pricePath = (key) => {
    const segs = [];
    let cur = null, prev = 0;
    for (const p of pts) {
      if (!finite(p[key])) continue;
      if (!cur || Number(p.t) - prev > gapMs) { cur = []; segs.push(cur); }
      cur.push([X(p.t), YP(p[key])]);
      prev = Number(p.t);
    }
    return segs.map((seg) => (seg.length === 1
      ? `M${f1(seg[0][0])} ${f1(seg[0][1])} L${f1(seg[0][0])} ${f1(seg[0][1])}`
      : seg.map(([x, y], i) => `${i ? 'L' : 'M'}${f1(x)} ${f1(y)}`).join(' '))).join(' ');
  };
  // Ступенчатая линия накопленного объёма: (начало окна, 0) → скачок в момент каждой точки → продолжение до конца окна.
  const volumePath = (field) => {
    const own = pts.filter((p) => finite(p[field]));
    if (own.length === 0) return '';
    let d = `M${f1(X(t0))} ${f1(YV(0))}`;
    let y = YV(0);
    for (const p of own) {
      const x = X(p.t);
      d += ` L${f1(x)} ${f1(y)}`;            // горизонталь до момента сделок на прежнем уровне
      y = YV(p[field]);
      d += ` L${f1(x)} ${f1(y)}`;            // скачок
    }
    return `${d} L${f1(X(t1))} ${f1(y)}`;    // уровень сохраняется до конца окна
  };

  const span = t1 - t0;
  return {
    W, H, m, pw, ph, t0, t1, X, YV, YP, vScale, pScale, hasPrice: !!pScale, mode: vm.key,
    volumeLines: vm.fields.map(([field, label, color]) => ({ field, label, color, d: volumePath(field) })),
    priceLines: PRICE_SERIES.filter((s) => priceKeys.includes(s.key)).map((s) => ({ ...s, d: pricePath(s.key) })),
    yTicksV: niceTicks(vScale.lo, vScale.hi, 4).map((v) => ({ v, y: YV(v) })),
    yTicksP: pScale ? niceTicks(pScale.lo, pScale.hi, 4).map((v) => ({ v, y: YP(v) })) : [],
    xTicks: [0, 1, 2, 3, 4].map((i) => { const t = t0 + (span * i) / 4; return { t, x: X(t), label: fmtAxisTime(t, span) }; }),
  };
}
