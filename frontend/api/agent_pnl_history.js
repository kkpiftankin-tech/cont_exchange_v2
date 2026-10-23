'use strict';
// История заработка и выгоды агента во времени (график по клику на агента в панели «Живые позиции»).
// Ряд R/U — это ровно те числа, что панель показывает в колонке «заработок R / U»: BFF раз в минуту записывает их в
// ce_agent_pnl_history (realized, unrealized), график читает записанное. Выгода участника в клиринге (CALC-CE-CLEARING-SURPLUS,
// ADR-083) пишется в те же строки приращением за интервал (surplus_inc_usdt). Ничего не считается заново, поэтому график не может
// разойтись с таблицей. Чистые функции без ввода-вывода; запись и чтение PG — в server.js.

const finite = (v) => v !== null && v !== undefined && Number.isFinite(Number(v));

// rows — строки панели (agentRows); tsMs — момент выборки; surplus — результат drain() накопителя тактов
// ({ agents: Map(agent → { surplus, tacts }), tacts, expected }) или null. В выборку попадают агенты, у которых есть хотя бы R, U
// или выгода (нет данных остаётся «нет данных», а не нулём). R без U (или наоборот) пишется как есть, отсутствующее значение — null.
function pnlSamples(rows, tsMs, surplus) {
  const out = [];
  const seen = new Set();
  const sAgents = surplus && surplus.agents instanceof Map ? surplus.agents : new Map();
  const sExpected = surplus && finite(surplus.expected) ? Math.trunc(Number(surplus.expected)) : 0;
  const sPart = (id) => {
    const s = sAgents.get(id);
    if (!s || !finite(s.surplus)) return { surplus_inc: null, surplus_tacts: null, surplus_expected: null };
    // число тактов общее для всех участников (полнота опроса снимков клиринга), а не присутствие ребра в графе
    return { surplus_inc: Number(s.surplus), surplus_tacts: finite(surplus.tacts) ? Math.trunc(Number(surplus.tacts)) : 0, surplus_expected: sExpected };
  };
  for (const a of Array.isArray(rows) ? rows : []) {
    if (!a || !a.agent_id) continue;
    // как в панели: если R нет, а есть pnl_usdt (R+U), его используют вместо R
    const r = finite(a.realized_usdt) ? Number(a.realized_usdt) : (finite(a.pnl_usdt) ? Number(a.pnl_usdt) : null);
    const u = finite(a.unrealized_usdt) ? Number(a.unrealized_usdt) : null;
    const sp = sPart(a.agent_id);
    if (r === null && u === null && sp.surplus_inc === null) continue;
    seen.add(a.agent_id);
    out.push({
      agent_id: a.agent_id, ts_ms: Number(tsMs), realized: r, unrealized: u,
      fills: finite(a.trade_fills) ? Math.trunc(Number(a.trade_fills)) : 0, ...sp,
    });
  }
  // участники с выгодой, которых нет среди строк панели (например, связки без внешних сделок)
  for (const id of sAgents.keys()) {
    if (seen.has(id) || !id) continue;
    const sp = sPart(id);
    if (sp.surplus_inc === null) continue;
    out.push({ agent_id: id, ts_ms: Number(tsMs), realized: null, unrealized: null, fills: 0, ...sp });
  }
  return out;
}

// Прореживание: не больше maxPoints точек, в каждом интервале берётся ПОСЛЕДНЯЯ точка (кумулятивная кривая сохраняет конец
// интервала); первая и последняя точки ряда сохраняются. Порядок по времени.
function downsample(points, maxPoints) {
  const pts = (Array.isArray(points) ? points : []).slice().sort((x, y) => x.t - y.t);
  const cap = Math.max(2, Math.floor(maxPoints) || 2);
  if (pts.length <= cap) return pts;
  const t0 = pts[0].t, t1 = pts[pts.length - 1].t;
  const span = t1 - t0;
  if (!(span > 0)) return [pts[pts.length - 1]];
  const buckets = new Map();
  for (const p of pts) {
    const idx = Math.min(cap - 1, Math.floor(((p.t - t0) / span) * cap));
    buckets.set(idx, p);   // последняя точка интервала перекрывает предыдущие
  }
  const out = [...buckets.entries()].sort((x, y) => x[0] - y[0]).map((e) => e[1]);
  if (out[0].t !== pts[0].t) {
    if (out.length >= cap) out[0] = pts[0]; else out.unshift(pts[0]);   // начало ряда сохраняем, лимит точек не превышаем
  }
  return out;
}

// dbRows — строки PG { ts_ms, realized_usdt, unrealized_usdt, surplus_inc_usdt, surplus_tacts, surplus_tacts_expected } одного агента за
// окно. DTO для графика: R, U, R+U (total) и накопленная за окно выгода s. R+U считается только когда есть хотя бы одна часть; иначе null.
function buildPnlHistoryDto(agentId, dbRows, hours, nowMs, maxPoints = 500) {
  const raw = [];
  let sCum = 0, hasS = false, tactsSeen = 0, tactsExp = 0;
  const sorted = (Array.isArray(dbRows) ? dbRows : []).slice().sort((x, y) => Number(x.ts_ms) - Number(y.ts_ms));
  for (const r of sorted) {
    const t = Number(r.ts_ms);
    if (!Number.isFinite(t)) continue;
    const rr = finite(r.realized_usdt) ? Number(r.realized_usdt) : null;
    const uu = finite(r.unrealized_usdt) ? Number(r.unrealized_usdt) : null;
    const si = finite(r.surplus_inc_usdt) ? Number(r.surplus_inc_usdt) : null;
    if (rr === null && uu === null && si === null) continue;
    if (si !== null) {
      sCum += si; hasS = true;
      tactsSeen += finite(r.surplus_tacts) ? Number(r.surplus_tacts) : 0;
      tactsExp += finite(r.surplus_tacts_expected) ? Number(r.surplus_tacts_expected) : 0;
    }
    raw.push({
      t, r: rr, u: uu,
      total: (rr === null && uu === null) ? null : (rr === null ? 0 : rr) + (uu === null ? 0 : uu),
      s: hasS ? sCum : null,
    });
  }
  const points = downsample(raw, maxPoints);
  return {
    agent_id: agentId,
    window_hours: hours,
    since_ms: Number.isFinite(Number(nowMs)) ? Number(nowMs) - hours * 3600 * 1000 : null,
    first_ms: points.length ? points[0].t : null,
    points,
    has_data: points.length > 0,
    has_surplus: hasS,
    surplus_total: hasS ? sCum : null,
    surplus_coverage: tactsExp > 0 ? Math.min(1, tactsSeen / tactsExp) : null,
  };
}

module.exports = { pnlSamples, downsample, buildPnlHistoryDto };
