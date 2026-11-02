// Раскладка строк панели «Живые позиции»: группы по биржам и фиксированный порядок. Чистые функции без React.
// Требования владельца (2026-10-20): (1) переводчики и арбитражёры сгруппированы по биржам; (2) строки не прыгают — все агенты
// показаны сразу, в том числе не действующие в этом такте, и порядок не зависит от хода клиринга (двигался агент или нет).

const ASSET_ORDER = ['BTC', 'ETH', 'SOL'];
// Ранг актива в группе: сначала BTC, ETH, SOL, затем остальные по алфавиту (общий ранг 100), нумерарий USDT — последним.
const assetRank = (a) => {
  if (a === 'USDT') return 1000;
  const i = ASSET_ORDER.indexOf(a);
  return i < 0 ? 100 : i;
};
const cmpStr = (x, y) => String(x || '').localeCompare(String(y || ''));
const cmpRank = (a, b) => assetRank(a) - assetRank(b);
// Котируемая валюта: USDT (прямая пара) раньше кросс-пар.
const quoteRank = (q) => (!q || q === 'USDT' ? 0 : 1);

// Переводчик: биржа → актив → котируемая (USDT первой) → agent_id.
export function compareTranslators(a, b) {
  return cmpStr(a.venue, b.venue)
    || cmpRank(a.base, b.base)
    || (quoteRank(a.quote) - quoteRank(b.quote))
    || cmpStr(a.quote, b.quote)
    || cmpStr(a.agent_id, b.agent_id);
}

// Арбитражёр: биржа-источник → биржа-приёмник → актив → agent_id.
export function compareArbitrageurs(a, b) {
  const ra = a.arb || {}, rb = b.arb || {};
  return cmpStr(ra.src, rb.src)
    || cmpStr(ra.dst, rb.dst)
    || cmpRank(a.base, b.base)
    || cmpStr(a.agent_id, b.agent_id);
}

// Группы переводчиков по бирже, порядок групп — по алфавиту, внутри группы — compareTranslators.
export function groupTranslators(rows) {
  const t = (rows || []).filter((r) => r.agent_kind === 'translator').slice().sort(compareTranslators);
  const groups = [];
  for (const r of t) {
    const venue = r.venue || '';
    let g = groups[groups.length - 1];
    if (!g || g.venue !== venue) { g = { venue, rows: [] }; groups.push(g); }
    g.rows.push(r);
  }
  return groups;
}

// Группы арбитражёров по паре бирж «источник ↔ приёмник» (один арбитражёр перевозит актив между двумя биржами и входит
// в одну группу — позиции не дублируются между биржами); порядок групп — по алфавиту.
export function groupArbitrageurs(rows) {
  const a = (rows || []).filter((r) => r.agent_kind === 'arbitrageur').slice().sort(compareArbitrageurs);
  const groups = [];
  for (const r of a) {
    const src = (r.arb && r.arb.src) || '', dst = (r.arb && r.arb.dst) || '';
    const key = src || dst ? `${src || '?'} ↔ ${dst || '?'}` : 'без маршрута';
    let g = groups[groups.length - 1];
    if (!g || g.key !== key) { g = { key, src, dst, rows: [] }; groups.push(g); }
    g.rows.push(r);
  }
  return groups;
}

// Агент не действует: нет позиции и не двигался в этом такте. Для строки показа (приглушается), не для отбора.
export function isIdleAgent(r) {
  if (!r || r.hasDelta) return false;
  if (r.agent_kind === 'arbitrageur') {
    const a = r.arb || {};
    return !(Math.abs(Number(a.leg_src) || 0) > 1e-9 || Math.abs(Number(a.leg_dst) || 0) > 1e-9);
  }
  return !(Math.abs(Number(r.c_total != null ? r.c_total : r.c_position) || 0) > 1e-9);
}

// Живая панель: агент, который уже был показан, не исчезает, даже если в очередном ответе его нет (например, граф клиринга на
// такт потерял пару). Его последняя известная строка остаётся, помечена stale и без дельты такта. Порядок строк от этого не
// зависит (его задаёт сортировка), так что ничего не «прыгает».
export function mergeStableRows(prevRows, nextRows) {
  const next = Array.isArray(nextRows) ? nextRows : [];
  const seen = new Set(next.map((r) => r.agent_id));
  const kept = (Array.isArray(prevRows) ? prevRows : [])
    .filter((r) => r && !seen.has(r.agent_id))
    .map((r) => ({
      ...r, stale: true, hasDelta: false,
      delta_base: 0, delta_quote: 0, c_delta: 0,
      arb: r.arb ? { ...r.arb, dleg_src: 0, dleg_dst: 0 } : r.arb,
    }));
  return [...next.map((r) => (r.stale ? { ...r, stale: false } : r)), ...kept];
}
