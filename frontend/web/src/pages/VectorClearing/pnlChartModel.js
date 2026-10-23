// Модель графика заработка агента во времени: только шкалы, подписи осей и пути линий (презентация). Сами числа R, U и R+U приходят
// готовыми из BFF (/api/vector-clearing/agent-pnl-history) — здесь ничего не пересчитывается (frontend-no-domain-compute).

const finite = (v) => v !== null && v !== undefined && Number.isFinite(Number(v));

// «Красивые» отметки оси: ~count штук с шагом 1/2/5·10^k, покрывающие [min, max].
export function niceTicks(min, max, count = 4) {
  if (!(max > min)) return [min];
  const rough = (max - min) / Math.max(1, count);
  const pow = Math.pow(10, Math.floor(Math.log10(rough)));
  const frac = rough / pow;
  const step = (frac <= 1 ? 1 : frac <= 2 ? 2 : frac <= 5 ? 5 : 10) * pow;
  const out = [];
  for (let v = Math.ceil(min / step) * step; v <= max + step * 1e-9; v += step) out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  return out;
}

// USDT с знаком для подписей и всплывающих значений: крупные без дробной части, мелкие с двумя знаками.
export function fmtUsdt(v, signed = false) {
  if (!finite(v)) return '—';
  const n = Number(v);
  const a = Math.abs(n);
  const body = a >= 1000 ? n.toLocaleString('ru-RU', { maximumFractionDigits: 0 }) : a >= 10 ? n.toFixed(1) : n.toFixed(2);
  return signed && n > 0 ? '+' + body : body;
}

// Подпись времени: в пределах суток — ЧЧ:ММ, на длинном окне — «дд.мм ЧЧ:ММ».
export function fmtAxisTime(ms, spanMs) {
  const d = new Date(Number(ms));
  if (Number.isNaN(d.getTime())) return '—';
  const hm = d.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit', hour12: false });
  if (spanMs <= 24 * 3600 * 1000) return hm;
  return d.toLocaleDateString('ru-RU', { day: '2-digit', month: '2-digit' }) + ' ' + hm;
}

// Ряды графика: ключ в точке, подпись, цвет, пояснение (для чекбоксов и легенды).
export const PNL_SERIES = [
  { key: 'r', label: 'R', color: '#5fd08a', hint: 'R — реализованный заработок: уже зафиксирован сделками' },
  { key: 'u', label: 'U', color: '#e0b84a', hint: 'U — нереализованный результат: остаток по текущим ценам, меняется с рынком' },
  { key: 'total', label: 'R + U', color: '#4fb0d8', hint: 'R + U — полный результат по текущим ценам' },
  { key: 's', label: 'S выгода', color: '#b58cff', hint: 'S — накопленная за выбранное окно выгода участника в клиринге по теории (w²/2α по тактам). Это оценка относительно кривой агента, а не деньги в кассе' },
];
export const DEFAULT_PNL_VISIBLE = { r: true, u: false, total: true, s: false };

// points: [{ t, r, u, total }] по возрастанию t (r/u могут быть null). visible: какие ряды отмечены галочками.
// Ось Y подгоняется под ОТМЕЧЕННЫЕ ряды (а не включает ноль всегда): иначе малый R рядом с большим U выглядит прямой.
// Ноль рисуется, только если он внутри диапазона (zeroY === null иначе). Возвращает null, если рисовать нечего.
export function buildPnlChartModel(points, W, H, visible = DEFAULT_PNL_VISIBLE, m = { l: 62, r: 12, t: 12, b: 28 }) {
  const pts = (Array.isArray(points) ? points : []).filter((p) => finite(p && p.t));
  if (pts.length === 0) return null;
  const pw = W - m.l - m.r, ph = H - m.t - m.b;
  let t0 = Number(pts[0].t), t1 = Number(pts[pts.length - 1].t);
  if (!(t1 > t0)) { t0 -= 30000; t1 += 30000; }   // одна точка: раздвигаем окно, чтобы шкала была определена
  const keys = PNL_SERIES.map((s) => s.key).filter((k) => visible && visible[k]);
  let lo = Infinity, hi = -Infinity;
  for (const p of pts) {
    for (const k of keys) if (finite(p[k])) { lo = Math.min(lo, Number(p[k])); hi = Math.max(hi, Number(p[k])); }
  }
  if (!(lo <= hi)) { lo = -1; hi = 1; }             // ни одного отмеченного значения: пустые оси
  if (!(hi - lo > 1e-12)) {                          // плоский ряд (или одна точка): раздвигаем, чтобы шкала была определена
    const c = Math.max(Math.abs(hi) * 0.02, 1);
    lo -= c; hi += c;
  }
  const pad = (hi - lo) * 0.08;
  const yMin = lo - pad, yMax = hi + pad;
  const X = (t) => m.l + ((Number(t) - t0) / (t1 - t0)) * pw;
  const Y = (v) => m.t + (1 - (Number(v) - yMin) / (yMax - yMin)) * ph;
  // путь серии; при пропуске значения (null) линия разрывается, а не соединяет через дыру
  const path = (key) => {
    let d = '', pen = false;
    for (const p of pts) {
      if (!finite(p[key])) { pen = false; continue; }
      d += `${pen ? 'L' : 'M'}${X(p.t).toFixed(1)} ${Y(p[key]).toFixed(1)} `;
      pen = true;
    }
    return d.trim();
  };
  const span = t1 - t0;
  return {
    W, H, m, pw, ph, t0, t1, yMin, yMax, X, Y, zeroY: (yMin <= 0 && yMax >= 0) ? Y(0) : null,
    pathR: keys.includes('r') ? path('r') : '',
    pathU: keys.includes('u') ? path('u') : '',
    pathTotal: keys.includes('total') ? path('total') : '',
    pathS: keys.includes('s') ? path('s') : '',
    hasVisible: keys.length > 0,
    yTicks: niceTicks(yMin, yMax, 4).map((v) => ({ v, y: Y(v) })),
    xTicks: [0, 1, 2, 3, 4].map((i) => { const t = t0 + (span * i) / 4; return { t, x: X(t), label: fmtAxisTime(t, span) }; }),
    span,
  };
}

// Ближайшая к моменту t точка (для всплывающего значения под курсором).
export function nearestPoint(points, t) {
  let best = null, bd = Infinity;
  for (const p of Array.isArray(points) ? points : []) {
    const d = Math.abs(Number(p.t) - Number(t));
    if (d < bd) { bd = d; best = p; }
  }
  return best;
}
