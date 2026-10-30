'use strict';
// Панель «Живые позиции»: заявки агентов на внешние биржи. Итоги берём из child_orders (накопленное исполнение по каждой заявке,
// включая дозаполнение резидентных лимитов ADR-067), а не из venue_fill_diagnostics (она фиксирует только первый такт заявки и
// занижает исполнение резидентных заявок в разы). Только перенос и разметка строк БД: домен не пересчитываем.

// Статусы child_orders → слова, которыми панель описывала заявки раньше (NEW | PARTIALLY_FILLED | FILLED | EXPIRED | REJECTED).
const STATUS_MAP = { PENDING: 'NEW', CANCELLED: 'EXPIRED' };
const mapChildStatus = (s) => (STATUS_MAP[s] || s || null);

const num = (v) => {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
};

// rows — последняя заявка по каждой паре (агент, биржа): { agent_id, venue, symbol, side, target_qty, filled_qty, status, created_ms }.
// Возвращает Map(agent_id → поля последней заявки агента + hedge_by_venue). Поля верхнего уровня описывают самую свежую заявку агента
// (как раньше), hedge_by_venue — последнюю заявку на каждой из бирж (арбитражёр хеджирует обе ноги).
function buildHedgeMap(rows) {
  const out = new Map();
  for (const r of Array.isArray(rows) ? rows : []) {
    if (!r || !r.agent_id) continue;
    const entry = {
      hedge_symbol: r.symbol, hedge_side: r.side, hedge_venue: r.venue || '',
      hedge_sent: num(r.target_qty), hedge_filled: num(r.filled_qty),
      hedge_status: mapChildStatus(r.status), hedge_ms: num(r.created_ms),
    };
    if (!out.has(r.agent_id)) out.set(r.agent_id, { hedge_by_venue: {} });
    const agg = out.get(r.agent_id);
    agg.hedge_by_venue[entry.hedge_venue] = {
      symbol: entry.hedge_symbol, side: entry.hedge_side, sent: entry.hedge_sent, filled: entry.hedge_filled,
      status: entry.hedge_status, ms: entry.hedge_ms,
    };
    if (agg.hedge_ms === undefined || entry.hedge_ms >= agg.hedge_ms) Object.assign(agg, entry);
  }
  return out;
}

module.exports = { buildHedgeMap, mapChildStatus };
