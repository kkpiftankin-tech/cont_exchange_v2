'use strict';
// ADR-082 (шаг 0): слой измерения ликвидности бирж → DTO для экрана «Внешние биржи» (GET /api/venues/liquidity-stats).
// Только перенос и разметка строк PG venue_liquidity_stats (symbol → base/quote, возраст записи, «есть ли данные»):
// никаких доменных расчётов — ν и f считает venues (cpp/venues/src/domain/liquidity_stats.hpp), экран показывает как есть.
// Нет данных ⇒ has_* = false и числа null (UI рисует «—», ничего не выдумываем).

// Котируемые валюты в порядке проверки суффикса (длинные раньше коротких: USDT до USD).
const QUOTES = ['USDT', 'USDC', 'USD', 'EUR', 'BTC', 'ETH'];

const num = (v) => {
  const n = typeof v === 'string' ? Number(v) : v;
  return typeof n === 'number' && Number.isFinite(n) ? n : null;
};

// Канонический ключ ленты (BTCUSDT, ETHBTC) → { base, quote }; незнакомый суффикс ⇒ quote=null, base=весь символ.
function splitSymbol(symbol) {
  const s = String(symbol || '').toUpperCase();
  for (const q of QUOTES) {
    if (s.length > q.length && s.endsWith(q)) return { base: s.slice(0, -q.length), quote: q };
  }
  return { base: s, quote: null };
}

// rows — строки venue_liquidity_stats (pg отдаёт bigint строкой, updated_at — Date или ISO-строкой).
// staleMs — старше этого запись считается «не обновляется» (venues пишет раз в ~10 с).
function buildLiquidityStatsView(rows, nowMs, staleMs = 120000) {
  const byVenue = new Map();
  for (const r of Array.isArray(rows) ? rows : []) {
    if (!r || !r.venue || !r.symbol) continue;
    const { base, quote } = splitSymbol(r.symbol);
    const samples = num(r.tape_samples) || 0;
    const orders = num(r.fill_orders) || 0;
    const rate = num(r.tape_quote_per_s);
    const updatedMs = r.updated_at ? new Date(r.updated_at).getTime() : NaN;
    const ageMs = Number.isFinite(updatedMs) ? Math.max(0, nowMs - updatedMs) : null;
    const hasTape = samples > 0 && rate != null;
    const hasFill = orders > 0;
    const pair = {
      symbol: r.symbol,
      base,
      quote,
      has_tape: hasTape,
      tape_quote_per_s: hasTape ? rate : null,
      tape_quote_per_min: hasTape ? rate * 60 : null,
      tape_samples: samples,
      has_fill: hasFill,
      fill_coef: hasFill ? num(r.fill_coef) : null,
      fill_weight: hasFill ? num(r.fill_weight) : null,
      fill_orders: orders,
      updated_at_ms: Number.isFinite(updatedMs) ? updatedMs : null,
      age_ms: ageMs,
      stale: ageMs == null ? true : ageMs > staleMs,
    };
    if (!byVenue.has(r.venue)) byVenue.set(r.venue, []);
    byVenue.get(r.venue).push(pair);
  }
  return Array.from(byVenue.entries())
    .map(([venue, pairs]) => ({ venue, pairs: pairs.sort((a, b) => a.symbol.localeCompare(b.symbol)) }))
    .sort((a, b) => a.venue.localeCompare(b.venue));
}

module.exports = { buildLiquidityStatsView, splitSymbol };
