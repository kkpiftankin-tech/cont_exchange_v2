'use strict';
// ADR-087 (CALC-CE-AGENT-PNL): ответ ledger GetAgentPnl → JSON для вкладки «PnL».
// Это ТОЛЬКО пересылка: переименование полей и приведение типов proto (Decimal {units, scale} → число для отображения,
// int64-строки → числа). Арифметики над R и U здесь нет и быть не должно — R, U, R+U, итоги по группам и строка «все агенты»
// уже посчитаны в ledger (деньги — Decimal, CLAUDE.md §1). Нет данных (has_data=false) отдаётся null, а не нулём.

const num = (v) => {
  if (v === null || v === undefined || v === '') return null;
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
};

// proto Decimal {units, scale} → number (units·10^-scale) для отображения; нет значения — null.
function decOrNull(d) {
  if (!d) return null;
  const u = Number(d.units);
  const s = Number(d.scale || 0);
  if (!Number.isFinite(u) || !Number.isFinite(s)) return null;
  return u / Math.pow(10, s);
}

function groupDto(g) {
  if (!g || !g.has_data) return { has_data: false, r: null, u: null, r_plus_u: null };
  return { has_data: true, r: decOrNull(g.r), u: decOrNull(g.u), r_plus_u: decOrNull(g.r_plus_u) };
}

const KIND_ORDER = { translator: 0, arbitrageur: 1 };

function agentDto(a) {
  const age = num(a && a.mark_age_ms);
  const since = num(a && a.since_ms);
  return {
    agent_id: a.agent_id || '',
    agent_kind: a.agent_kind || '',
    asset: a.asset || '',
    quote: a.quote || '',
    venues: Array.isArray(a.venues) ? a.venues.slice() : [],
    internal: groupDto(a.internal),
    external: groupDto(a.external),
    total: groupDto(a.total),
    mark_age_ms: age !== null && age >= 0 ? age : null,   // -1 в ответе ledger — цена неизвестна
    ext_fills: num(a.ext_fills) || 0,
    since_ms: since !== null && since > 0 ? since : null, // 0 — накопление не начато
  };
}

// resp — ответ GetAgentPnl (proto-loader, keepCase, longs: String). now — ISO-метка времени ответа.
function buildAgentPnlDto(resp, now) {
  const r = resp || {};
  const agents = (Array.isArray(r.agents) ? r.agents : []).map(agentDto).sort((x, y) => {
    const kx = KIND_ORDER[x.agent_kind] != null ? KIND_ORDER[x.agent_kind] : 2;
    const ky = KIND_ORDER[y.agent_kind] != null ? KIND_ORDER[y.agent_kind] : 2;
    if (kx !== ky) return kx - ky;
    return String(x.agent_id).localeCompare(String(y.agent_id));
  });
  const t = r.totals || {};
  return {
    currency: r.currency || 'USDT',
    enabled: r.enabled !== false,
    agents,
    totals: {
      internal: groupDto(t.internal),
      external: groupDto(t.external),
      total: groupDto(t.total),
      agent_count: num(t.agent_count) || 0,
      incomplete_agents: num(t.incomplete_agents) || 0,
    },
    generatedAt: now || new Date().toISOString(),
  };
}

module.exports = { buildAgentPnlDto, groupDto, agentDto, decOrNull };
