'use strict';
// Этап 0 (EXP-2026-10-15-exchange-requests): сливает тайминги стадий опроса из админ-API
// venues (/api/v1/venues → items[].metrics) в ответ /api/venues/stats. Только перенос чисел,
// без пересчётов; нет данных venues ⇒ поля null (UI показывает «—», ничего не выдумываем).

const STAGE_FIELDS = [
  'lock_wait_ms', 'fetch_ms', 'rest_depth_ms', 'rest_ticker_ms', 'rest_trades_ms',
  'raw_publish_ms', 'snapshot_ms', 'curve_ms', 'total_ms',
];

const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null);

function mergeVenueStages(venues, adminItems, nowMs) {
  const metricsByVenue = new Map();
  for (const it of Array.isArray(adminItems) ? adminItems : []) {
    const id = it && it.config && it.config.venue_id;
    if (id && it.metrics && typeof it.metrics === 'object') metricsByVenue.set(id, it.metrics);
  }
  return (Array.isArray(venues) ? venues : []).map((v) => {
    const m = metricsByVenue.get(v.venue_id);
    if (!m) {
      return { ...v, cycle_ms: null, cycle_ms_ema: null, cycle_end_ms: null, stages_available: false };
    }
    const stages = m.stages && typeof m.stages === 'object' ? m.stages : {};
    // До первого завершённого прохода venues отдаёт нули — это «нет данных», а не 0 мс.
    const cycleDone = num(m.cycle_end_ms) > 0;
    const pairs = (v.pairs || []).map((p) => {
      const st = stages[p.symbol];
      if (!st || typeof st !== 'object') return { ...p, stages: null };
      const out = {};
      for (const f of STAGE_FIELDS) out[f] = num(st[f]);
      out.rest_requests = num(st.rest_requests);
      out.ticker_state = num(st.ticker_state);
      const recv = num(st.recv_ms);
      out.book_age_ms = recv && recv > 0 && Number.isFinite(nowMs) ? Math.max(0, nowMs - recv) : null;
      return { ...p, stages: out };
    });
    return {
      ...v,
      cycle_ms: cycleDone ? num(m.cycle_ms) : null,
      cycle_ms_ema: cycleDone ? num(m.cycle_ms_ema) : null,
      cycle_end_ms: cycleDone ? num(m.cycle_end_ms) : null,
      stages_available: true,
      pairs,
    };
  });
}

module.exports = { mergeVenueStages, STAGE_FIELDS };
