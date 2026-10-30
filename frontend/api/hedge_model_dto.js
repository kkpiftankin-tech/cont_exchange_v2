'use strict';
// Модельные пороги хеджа агентов (ADR-088, CALC-CE-HEDGE-THRESHOLD-MODEL): выдача строки панели и история во времени.
// Ledger считает пороги и оценки потока и отдаёт их в поле AgentPosition.hedge_model; BFF ничего не пересчитывает:
// переводит Decimal в числа, различает «не посчитано» (null) и законный ноль (ребейт мейкера даёт b_m = 0, чисто
// фиксированная плата сети — цель перевода 0), собирает строки минутной истории ce_agent_band_history и CSV для выгрузки.
// Чистые функции без ввода-вывода; запись и чтение PG — в server.js.

const finite = (v) => v !== null && v !== undefined && v !== '' && Number.isFinite(Number(v));

// Decimal из proto-loader (units — строка при longs: String): null, если поля нет (message-поле не заполнено ledger'ом).
// В отличие от decToNum в server.js, отсутствие НЕ превращается в 0: ноль — законное значение, null — «не посчитано».
function decOrNull(d) {
  if (!d || typeof d !== 'object') return null;
  const u = Number(d.units);
  const s = Number(d.scale || 0);
  if (!Number.isFinite(u) || !Number.isFinite(s)) return null;
  return u / Math.pow(10, s);
}

const REGIME = { HEDGE_MODEL_REGIME_NOISE: 'noise', HEDGE_MODEL_REGIME_MIXED: 'mixed', HEDGE_MODEL_REGIME_DRIFT: 'drift' };

// hm — сообщение AgentHedgeModel из ответа GetAgentPositions (после proto-loader: keepCase, defaults, enums как строки) или null.
// nowMs — для возраста расчёта (считает BFF как возраст, не как расчёт модели).
function buildHedgeModelDto(hm, nowMs) {
  if (!hm || typeof hm !== 'object') return null;
  const computed = hm.computed === true;
  const at = finite(hm.computed_at_ms) ? Number(hm.computed_at_ms) : 0;
  const peclet = finite(hm.peclet) ? Number(hm.peclet) : null;
  return {
    mode: typeof hm.mode === 'string' && hm.mode ? hm.mode : null,                 // 'shadow' | 'on'
    computed,
    applied: hm.applied === true,                                                   // режим on: пороги модели применены к заявкам
    reason: hm.reason ? String(hm.reason) : null,                                   // no_sigma, sigma_samples, sigma_stale, invalid_input, budget; у удержанных (computed) — <причина>_hold
    regime: computed ? (REGIME[hm.regime] || null) : null,                          // только у переводчика
    b_m: computed ? decOrNull(hm.b_m) : null,                                       // ЭФФЕКТИВНЫЙ порог (сглаженный, с потолком) — он идёт в заявки в режиме on
    b_t: computed ? decOrNull(hm.b_t) : null,
    b_m_raw: computed ? decOrNull(hm.b_m_raw) : null,                               // сырой расчёт этого такта (до сглаживания)
    b_t_raw: computed ? decOrNull(hm.b_t_raw) : null,
    sz_est: decOrNull(hm.sz_est),                                                   // шум потока, тыс. USDT за √мин
    mu_est: decOrNull(hm.mu_est),                                                   // дрейф, знаковый, тыс. USDT/мин
    theta_est: decOrNull(hm.theta_est),                                             // скорость возврата (диагностика), 1/мин
    peclet: computed && peclet !== null ? peclet : null,
    sigma_bps: computed ? decOrNull(hm.sigma_bps) : null,                           // волатильность курса пары, bps за √мин
    fee_maker_bps: computed ? decOrNull(hm.fee_maker_bps) : null,
    fee_taker_bps: computed ? decOrNull(hm.fee_taker_bps) : null,
    transfer_threshold: computed ? decOrNull(hm.transfer_threshold) : null,         // B, только связка
    transfer_target: computed ? decOrNull(hm.transfer_target) : null,               // b, только связка
    transfer_threshold_raw: computed ? decOrNull(hm.transfer_threshold_raw) : null, // сырые B и b связки
    transfer_target_raw: computed ? decOrNull(hm.transfer_target_raw) : null,
    mode_ceiling: typeof hm.mode_ceiling === 'string' && hm.mode_ceiling ? hm.mode_ceiling : null,   // потолок окружения ledger: off | shadow | on (mode — эффективный)
    est_minutes: finite(hm.est_minutes) ? Math.trunc(Number(hm.est_minutes)) : 0,   // 0 ⇒ шум по умолчанию, не оценка
    params_hash: finite(hm.params_hash) ? Number(hm.params_hash) : null,
    computed_at_ms: at || null,
    age_ms: at && finite(nowMs) ? Math.max(0, Number(nowMs) - at) : null,
  };
}

// Строка истории из строки панели (agentRows): рабочие пороги (z_lim, z_mkt) и модельные рядом. Агенты без модельного блока
// (режим off) в историю не идут: ряд рабочих порогов без модели не нужен.
function bandSamples(rows, tsMs) {
  const out = [];
  for (const a of Array.isArray(rows) ? rows : []) {
    if (!a || !a.agent_id || !a.hmodel) continue;
    const h = a.hmodel;
    out.push({
      agent_id: a.agent_id,
      ts_ms: Number(tsMs),
      kind: a.agent_kind || null,
      mode: h.mode,
      position: finite(a.c_position) ? Number(a.c_position) : null,
      in_flight: finite(a.in_flight) ? Number(a.in_flight) : null,
      z_lim: finite(a.z_lim) ? Number(a.z_lim) : null,
      z_mkt: finite(a.z_mkt) ? Number(a.z_mkt) : null,
      b_m: h.b_m, b_t: h.b_t, b_m_raw: h.b_m_raw, b_t_raw: h.b_t_raw, regime: h.regime, computed: h.computed, applied: h.applied, reason: h.reason,
      sz_est: h.sz_est, mu_est: h.mu_est, theta_est: h.theta_est, peclet: h.peclet, sigma_bps: h.sigma_bps,
      fee_maker_bps: h.fee_maker_bps, fee_taker_bps: h.fee_taker_bps,
      transfer_threshold: h.transfer_threshold, transfer_target: h.transfer_target,
      transfer_threshold_raw: h.transfer_threshold_raw, transfer_target_raw: h.transfer_target_raw,
      est_minutes: h.est_minutes, params_hash: h.params_hash,
    });
  }
  return out;
}

// Минутная метка: строка истории одна на агента и минуту (идемпотентность записи при повторе прохода).
function minuteBucketMs(ms) { return Math.floor(Number(ms) / 60000) * 60000; }

// Порядок прежних 25 колонок не менялся: сырые пороги дописаны в конец, чтобы позиционные разборы выгрузок эксперта (`$N`, `iloc`) не сломались.
const CSV_COLUMNS = ['ts', 'agent_id', 'kind', 'mode', 'position', 'in_flight', 'z_lim', 'z_mkt', 'b_m', 'b_t', 'regime', 'computed', 'applied',
  'reason', 'sz_est', 'mu_est', 'theta_est', 'peclet', 'sigma_bps', 'fee_maker_bps', 'fee_taker_bps', 'transfer_threshold', 'transfer_target',
  'est_minutes', 'params_hash', 'b_m_raw', 'b_t_raw', 'transfer_threshold_raw', 'transfer_target_raw'];

function csvCell(v) {
  if (v === null || v === undefined) return '';
  const s = String(v);
  return /[",\r\n;]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

// rows — строки из PG (поля как в CSV_COLUMNS, ts — Date или ISO). Для выгрузки эксперту (docs/experiments).
function historyCsv(rows) {
  const lines = [CSV_COLUMNS.join(',')];
  for (const r of Array.isArray(rows) ? rows : []) {
    lines.push(CSV_COLUMNS.map((c) => csvCell(c === 'ts' && r.ts instanceof Date ? r.ts.toISOString() : r[c])).join(','));
  }
  return lines.join('\n') + '\n';
}

// Схема таблицы истории. Создаёт BFF (единственный писатель, как у ce_agent_pnl_history); деньги — NUMERIC, метрики — DOUBLE PRECISION.
const BAND_HISTORY_DDL = `CREATE TABLE IF NOT EXISTS ce_agent_band_history (
  agent_id           TEXT NOT NULL,
  ts                 TIMESTAMPTZ NOT NULL,
  kind               TEXT,
  mode               TEXT,
  position           NUMERIC(38, 12),
  in_flight          NUMERIC(38, 12),
  z_lim              NUMERIC(38, 12),
  z_mkt              NUMERIC(38, 12),
  b_m                NUMERIC(38, 12),
  b_t                NUMERIC(38, 12),
  b_m_raw            NUMERIC(38, 12),
  b_t_raw            NUMERIC(38, 12),
  regime             TEXT,
  computed           BOOLEAN NOT NULL DEFAULT FALSE,
  applied            BOOLEAN NOT NULL DEFAULT FALSE,
  reason             TEXT,
  sz_est             DOUBLE PRECISION,
  mu_est             DOUBLE PRECISION,
  theta_est          DOUBLE PRECISION,
  peclet             DOUBLE PRECISION,
  sigma_bps          DOUBLE PRECISION,
  fee_maker_bps      DOUBLE PRECISION,
  fee_taker_bps      DOUBLE PRECISION,
  transfer_threshold NUMERIC(38, 12),
  transfer_target    NUMERIC(38, 12),
  transfer_threshold_raw NUMERIC(38, 12),
  transfer_target_raw    NUMERIC(38, 12),
  est_minutes        INTEGER NOT NULL DEFAULT 0,
  params_hash        BIGINT,
  PRIMARY KEY (agent_id, ts))`;
const BAND_HISTORY_INDEX = 'CREATE INDEX IF NOT EXISTS idx_ce_band_hist_ts ON ce_agent_band_history (ts)';
// Колонки сырых порогов (поправка о сглаживании ADR-088) для уже созданной таблицы. Добавляет ensureBandHistoryReady (server.js) по образцу
// ensurePnlHistoryReady: сначала проверка каталога, один ALTER в транзакции с коротким lock_timeout (ALTER берёт эксклюзивную блокировку даже тогда,
// когда колонка есть, и без таймаута встал бы в очередь за долгим запросом).
const BAND_HISTORY_RAW_COLUMNS = [['b_m_raw', 'NUMERIC(38, 12)'], ['b_t_raw', 'NUMERIC(38, 12)'],
  ['transfer_threshold_raw', 'NUMERIC(38, 12)'], ['transfer_target_raw', 'NUMERIC(38, 12)']];

module.exports = { decOrNull, buildHedgeModelDto, bandSamples, minuteBucketMs, historyCsv, CSV_COLUMNS, BAND_HISTORY_DDL, BAND_HISTORY_INDEX, BAND_HISTORY_RAW_COLUMNS };
