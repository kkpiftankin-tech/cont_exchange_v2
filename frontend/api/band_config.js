'use strict';
// ADR-081 — настройки ширины полосы хеджа CE: ручной режим и автоматический по волатильности.
// Чистые функции (без PG и HTTP): разбор и проверка тела запроса, запись аудита. Домена здесь нет — только границы значений;
// фактическую ширину считает ledger (cpp/ledger/src/app/ledger_uc.cpp#detect_and_emit_band_breach_locked).

const MIN_Q = 0.1;           // тыс. USDT (100 USDT) — нижняя граница ручной ширины: ниже хедж шёл бы на пыль (LedgerUseCases::kBandManualMin)
const MAX_Q = 1000;          // тыс. USDT — верхняя граница (≈ весь USDT-капитал, CE_CAPITAL_SEED_USDT = 1 000 000; LedgerUseCases::kBandManualMax)
const BAND_FIELDS = [
  'ce_band_manual', 'ce_band_manual_q_lim', 'ce_band_manual_q_mkt',
  'ce_band_gamma_mode', 'ce_band_q_floor', 'ce_band_sigma_ref',
  'ce_band_vol_ratio_min', 'ce_band_vol_ratio_max',
  'ce_band_arb_scale',        // множитель порога выхода арбитражёров (ADR-081)
];

const isAbsent = (v) => v === undefined || v === null || v === '';
const toBool = (v) => {
  if (v === true || v === 'true' || v === 1 || v === '1') return true;
  if (v === false || v === 'false' || v === 0 || v === '0') return false;
  return null;
};
const toNum = (v) => (typeof v === 'number' || (typeof v === 'string' && v.trim() !== '') ? Number(v) : NaN);

// body — тело запроса; current — значения в базе (числа/булевы), нужны для проверки сочетаний при частичном обновлении.
// Возвращает { ok:true, values } (только присланные поля) или { ok:false, code, message }.
function parseBandConfig(body, current = {}) {
  const b = body || {};
  const values = {};
  const bad = (message) => ({ ok: false, code: 'band_invalid', message });

  for (const f of ['ce_band_manual', 'ce_band_gamma_mode']) {
    if (isAbsent(b[f])) continue;
    const v = toBool(b[f]);
    if (v === null) return bad(`${f}: ожидается true или false`);
    values[f] = v;
  }

  const numeric = (field, lo, hi, loOpen) => {
    if (isAbsent(b[field])) return null;
    const v = toNum(b[field]);
    if (!Number.isFinite(v) || (loOpen ? v <= lo : v < lo) || v > hi) {
      return bad(`${field}: ожидается число ${loOpen ? '>' : '≥'} ${lo} и ≤ ${hi}`);
    }
    values[field] = v;
    return null;
  };
  for (const [f, lo, hi, open] of [
    ['ce_band_manual_q_lim', MIN_Q, MAX_Q, false],
    ['ce_band_manual_q_mkt', MIN_Q, MAX_Q, false],
    ['ce_band_q_floor', 0, MAX_Q, false],   // тыс. USDT
    ['ce_band_sigma_ref', 0, 1, true],
    ['ce_band_vol_ratio_min', 0, 1000, true],
    ['ce_band_vol_ratio_max', 0, 1000, true],
    ['ce_band_arb_scale', 0.1, 20, false],   // как LedgerUseCases::kArbScaleMin/Max
  ]) {
    const err = numeric(f, lo, hi, open);
    if (err) return err;
  }

  // Сочетания проверяем по итоговым значениям (присланные поверх текущих).
  const fin = (f) => (f in values ? values[f] : current[f]);
  const lim = Number(fin('ce_band_manual_q_lim')), mkt = Number(fin('ce_band_manual_q_mkt'));
  const touchedManual = ['ce_band_manual', 'ce_band_manual_q_lim', 'ce_band_manual_q_mkt'].some((f) => f in values);
  if (touchedManual) {
    if (fin('ce_band_manual') === true || 'ce_band_manual_q_lim' in values || 'ce_band_manual_q_mkt' in values) {
      if (!(lim >= MIN_Q) || !(mkt >= lim) || mkt > MAX_Q) {
        return bad(`ручная ширина: нужно ${MIN_Q} ≤ Z̄lim ≤ Z̄mkt ≤ ${MAX_Q} тыс. USDT (сейчас Z̄lim=${lim}, Z̄mkt=${mkt})`);
      }
    }
  }
  const vmin = Number(fin('ce_band_vol_ratio_min')), vmax = Number(fin('ce_band_vol_ratio_max'));
  if (('ce_band_vol_ratio_min' in values || 'ce_band_vol_ratio_max' in values) && !(vmin <= vmax)) {
    return bad(`границы множителя волатильности: нужно min ≤ max (сейчас ${vmin} и ${vmax})`);
  }
  return { ok: true, values };
}

// Режим словами по итоговым значениям: ручной / по волатильности / от комиссии (как в ledger: manual важнее gamma_mode).
function bandMode(cfg) {
  if (cfg && cfg.ce_band_manual === true) return 'manual';
  if (cfg && cfg.ce_band_gamma_mode === true) return 'volatility';
  return 'fee';
}

// Приводит строку из PG (NUMERIC приходит строкой) к числам и булевым; отсутствующие колонки пропускает.
function normalizeBandRow(row) {
  const out = {};
  if (!row) return out;
  for (const f of BAND_FIELDS) {
    if (row[f] === undefined || row[f] === null) continue;
    out[f] = (f === 'ce_band_manual' || f === 'ce_band_gamma_mode') ? row[f] === true : Number(row[f]);
  }
  return out;
}

// Запись аудита: кто, что было, что стало (только изменившиеся поля). Пишется BFF в лог одной строкой JSON.
function auditRecord({ user, before, after, nowMs }) {
  const changed = {};
  for (const f of BAND_FIELDS) {
    if (after && f in after && (!before || before[f] !== after[f])) changed[f] = { from: before ? before[f] ?? null : null, to: after[f] };
  }
  return {
    ts: new Date(nowMs === undefined ? Date.now() : nowMs).toISOString(),
    level: 'INFO',
    msg: 'F-18 изменена ширина полосы хеджа (ADR-081)',
    user: user || 'unknown',
    mode_before: bandMode(before), mode_after: bandMode({ ...(before || {}), ...(after || {}) }),
    changed,
  };
}

// Защита от подделки запроса из браузера (CSRF) на операторском маршруте: нужен Content-Type: application/json
// (кросс-сайтовая форма и «простой» fetch его выставить не могут без preflight), а если есть Origin — его хост должен
// совпадать с хостом запроса (nginx передаёт Host как есть) или входить в OPERATOR_ALLOWED_ORIGINS (имена хостов через запятую).
// Это НЕ авторизация: запрос без Origin (curl) проходит — настоящая защита сети и токен оператора появятся с F-16/F-01.
function checkOperatorRequest(headers, allowedHosts = []) {
  const h = headers || {};
  const ct = String(h['content-type'] || '').toLowerCase();
  if (!ct.startsWith('application/json')) {
    return { ok: false, status: 415, code: 'unsupported_media_type', message: 'нужен Content-Type: application/json' };
  }
  const origin = h['origin'];
  if (origin) {
    let originHost;
    try { originHost = new URL(String(origin)).hostname; } catch (_) { originHost = null; }
    const reqHost = String(h['x-forwarded-host'] || h['host'] || '').replace(/:\d+$/, '').toLowerCase();
    const allowed = (allowedHosts || []).map((x) => String(x).trim().toLowerCase()).filter(Boolean);
    if (!originHost || (originHost.toLowerCase() !== reqHost && !allowed.includes(originHost.toLowerCase()))) {
      return { ok: false, status: 403, code: 'bad_origin', message: 'запрос с чужого источника отклонён' };
    }
  }
  return { ok: true };
}

module.exports = { parseBandConfig, bandMode, normalizeBandRow, auditRecord, checkOperatorRequest, BAND_FIELDS, MIN_Q, MAX_Q };
