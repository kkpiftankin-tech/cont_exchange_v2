// Общие для вкладки «Граф» цвета, форматтеры и шкалы анимации потока (страница CeGraphLive и карточки агентов AgentFlowCard).
// Только отображение: единицы, знаки и значения посчитаны бэкендом (ce_graph_build.js, CALC-CE-GRAPH).
export const VENUE_COLORS = { binance: '#f3ba2f', okx: '#7aa2ff', kraken: '#b08cff', coinbase: '#4aa3ff', uniswap_v3: '#ff6ec7' };
export const ASSET_COLORS = { USDT: '#26a17b', BTC: '#f7931a', ETH: '#8a9bff', SOL: '#14c9a5' };
export const vColor = (v) => VENUE_COLORS[v] || '#9aa7b4';
export const aColor = (a) => ASSET_COLORS[a] || '#9aa7b4';

export const fmt = (v, d = 4) => {
  const n = Number(v);
  if (!Number.isFinite(n)) return '—';
  const a = Math.abs(n);
  if (a !== 0 && (a < 1e-3 || a >= 1e7)) return n.toExponential(3);
  return n.toLocaleString('ru-RU', { maximumFractionDigits: a >= 1000 ? 2 : d });
};
export const sgn = (v, d = 3) => (Number.isFinite(Number(v)) ? (v > 0 ? '+' : '') + fmt(v, d) : '—');

// Количество валюты со знаком, компактно (для подписей у узла и внутри кружка): +1,23M / −12,3k / +0,0123 / 0.
// Минус — короткая позиция. Только отображение: единицы и знак посчитаны бэкендом.
export const fmtQty = (v) => {
  const n = Number(v);
  if (v == null || !Number.isFinite(n)) return '—';
  if (n === 0) return '0';
  const a = Math.abs(n), s = n > 0 ? '+' : '−';
  let t;
  if (a >= 1e6) t = (a / 1e6).toLocaleString('ru-RU', { maximumFractionDigits: 3 }) + 'M';
  else if (a >= 1e4) t = (a / 1e3).toLocaleString('ru-RU', { maximumFractionDigits: 2 }) + 'k';
  else if (a >= 100) t = a.toLocaleString('ru-RU', { maximumFractionDigits: 1 });
  else if (a >= 1) t = a.toLocaleString('ru-RU', { maximumFractionDigits: 3 });
  else t = Number(a.toPrecision(3)).toLocaleString('ru-RU', { maximumFractionDigits: 9 });
  return s + t;
};
// то же без знака («0,0123»): знак передаёт глагол подписи («продаёт», «получает»)
export const fmtQtyAbs = (v) => {
  const n = Number(v);
  if (v == null || !Number.isFinite(n)) return '—';
  if (n === 0) return '0';
  // микропотоки (доли цента ⇒ 1e-8 BTC) — экспонентой, иначе подпись ребра не помещается: «1,25e-8»
  return Math.abs(n) < 1e-4 ? Math.abs(n).toExponential(2).replace('.', ',') : fmtQty(Math.abs(n)).slice(1);
};
export const fmtUsd = (v) => { const a = Math.abs(Number(v)); return Number.isFinite(a) ? (a >= 100 ? fmt(a, 0) : a >= 1 ? fmt(a, 1) : fmt(a, 3)) : '—'; };

// Поток по ветви: положителен в направлении from → to. Скорость и число точек растут с |f| (логарифм).
// Масштаб относительно самого крупного потока видимых ветвей (потоки такта — доли и единицы USDT,
// абсолютная шкала их не различила бы): u = |f|/max ∈ (0,1].
export const flowU = (f, maxF) => (maxF > 0 ? Math.min(1, Math.abs(f) / maxF) : 0);
export const flowDur = (f, maxF) => 5.2 - 3.9 * Math.sqrt(flowU(f, maxF));
export const flowDots = (f, maxF) => 1 + Math.floor(3 * flowU(f, maxF));
export const flowW = (f, maxF) => 2 + 2.4 * Math.sqrt(flowU(f, maxF));
